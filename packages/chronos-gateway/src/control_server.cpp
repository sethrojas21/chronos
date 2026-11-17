#include "chronos/gateway/control_server.hpp"
#include "chronos/gateway/python_control.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace chronos::gateway {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;

std::string random_branch_id() {
    std::array<unsigned char, 12> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw GatewayError("branch id generation failed");
    std::ostringstream out;
    out << "rollout_" << std::hex << std::setfill('0');
    for (auto byte : bytes) out << std::setw(2) << static_cast<unsigned int>(byte);
    return out.str();
}

std::string state_name(AccessGrantState state) {
    switch (state) {
        case AccessGrantState::Provisioning: return "provisioning";
        case AccessGrantState::Active: return "active";
        case AccessGrantState::Revoked: return "closed";
        case AccessGrantState::Deleted: return "deleted";
    }
    return "unknown";
}

std::int64_t unix_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

Json branch_json(const AccessGrant &grant) {
    Json stores = Json::array();
    for (const auto &store : grant.postgres_stores) stores.push_back(store);
    return {
        {"workspace", grant.workspace},
        {"branch_id", grant.branch},
        {"sandbox", grant.owns_branch},
        {"databases", stores},
        {"state", state_name(grant.state)},
        {"expires_at", grant.expires() ? Json(unix_seconds(grant.expires_at)) : Json(nullptr)},
        {"sandbox_id", grant.sandbox_id},
        {"sandbox_ip", grant.sandbox_ip},
    };
}

// A long-lived branch without current data access.
Json branch_json(const std::string &workspace, const std::string &branch_id) {
    return {{"workspace", workspace}, {"branch_id", branch_id}, {"sandbox", false}};
}

FilesystemAccess filesystem_access(const Json &body) {
    const std::string filesystem = body.value("filesystem", std::string("none"));
    if (filesystem == "read_only") return FilesystemAccess::ReadOnly;
    if (filesystem == "read_write") return FilesystemAccess::ReadWrite;
    if (filesystem != "none") throw GatewayError("filesystem must be none, read_only, or read_write");
    return FilesystemAccess::None;
}

// ttl_seconds: absent gives fallback, null means no expiry.
std::optional<std::chrono::seconds> ttl(
    const Json &body,
    std::optional<std::chrono::seconds> fallback) {
    const auto found = body.find("ttl_seconds");
    if (found == body.end()) return fallback;
    if (found->is_null()) return std::nullopt;
    return std::chrono::seconds(found->get<long long>());
}

std::string metadata_json(const Json &body) {
    const auto found = body.find("metadata");
    if (found == body.end() || found->is_null()) return "{}";
    if (!found->is_object()) throw GatewayError("metadata must be a JSON object");
    return found->dump();
}

bool secure_equal(const std::string &left, const std::string &right) {
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string decode_path_component(const std::string &value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) throw GatewayError("invalid percent-encoded path");
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) throw GatewayError("invalid percent-encoded path");
        const char byte = static_cast<char>((high << 4) | low);
        if (byte == '\0') throw GatewayError("path component contains a null byte");
        decoded.push_back(byte);
        index += 2;
    }
    return decoded;
}

// key=value pairs of a URL query; '+' is a space and later keys win.
Json query_arguments(const std::string &query) {
    Json result = Json::object();
    std::size_t start = 0;
    while (start <= query.size()) {
        auto end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        std::string item = query.substr(start, end - start);
        for (auto &character : item) if (character == '+') character = ' ';
        const auto separator = item.find('=');
        const auto key = decode_path_component(item.substr(0, separator));
        if (!key.empty()) {
            result[key] = separator == std::string::npos
                ? std::string()
                : decode_path_component(item.substr(separator + 1));
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::string> path_parts(const std::string &target) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < target.size()) {
        while (start < target.size() && target[start] == '/') ++start;
        if (start == target.size()) break;
        auto end = target.find('/', start);
        parts.push_back(decode_path_component(target.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

} // namespace

class ControlServer::Impl {
  public:
    Impl(
        GatewayRuntime &runtime,
        ListenAddress listen,
        ListenAddress postgres,
        ListenAddress nfs,
        std::string token,
        const std::unordered_map<std::string, WorkspaceConfig> &workspaces)
        : runtime(runtime),
          listen(std::move(listen)),
          postgres(std::move(postgres)),
          nfs(std::move(nfs)),
          token(std::move(token)),
          python(workspaces),
          acceptor(io) {}

    ~Impl() { stop(); join_clients(); }

    void run() {
        const auto address = asio::ip::make_address(listen.host);
        tcp::endpoint endpoint(address, listen.port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(asio::socket_base::max_listen_connections);
        acceptor.non_blocking(true);
        while (!stopping.load()) {
            beast::error_code error;
            auto socket = std::make_shared<tcp::socket>(io);
            acceptor.accept(*socket, error);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (stopping.load() || error == asio::error::operation_aborted) break;
                throw beast::system_error(error);
            }
            std::lock_guard lock(clients_mutex);
            sockets.push_back(socket);
            clients.emplace_back([this, socket] { serve(socket); });
        }
        join_clients();
    }

    void stop() {
        if (stopping.exchange(true)) return;
        beast::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        std::lock_guard lock(clients_mutex);
        for (const auto &socket : sockets) {
            socket->cancel(ignored);
            socket->close(ignored);
        }
    }

    void join_clients() {
        std::vector<std::thread> pending;
        {
            std::lock_guard lock(clients_mutex);
            pending.swap(clients);
            sockets.clear();
        }
        for (auto &thread : pending) if (thread.joinable()) thread.join();
    }

    void serve(const std::shared_ptr<tcp::socket> &socket_ptr) noexcept {
        auto &socket = *socket_ptr;
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code error;
        http::read(socket, buffer, request, error);
        if (error) return;
        auto response = dispatch(request);
        response.keep_alive(false);
        http::write(socket, response, error);
        socket.shutdown(tcp::socket::shutdown_both, error);
    }

    http::response<http::string_body> dispatch(
        const http::request<http::string_body> &request) {
        if (request.method() == http::verb::get && request.target() == "/healthz") {
            return json_response(http::status::ok, {{"status", "ok"}});
        }
        const std::string authorization = request[http::field::authorization].to_string();
        if (!secure_equal(authorization, "Bearer " + token)) {
            return json_response(http::status::unauthorized, {{"error", "unauthorized"}});
        }
        try {
            std::string target(request.target());
            std::string query;
            const auto query_start = target.find('?');
            if (query_start != std::string::npos) {
                query = target.substr(query_start + 1);
                target.resize(query_start);
            }
            const auto parts = path_parts(target);
            if (auto routed = versioning(parts, request, query)) return std::move(*routed);
            if (parts.size() == 4 && parts[0] == "v1" && parts[1] == "workspaces" &&
                parts[3] == "branches" && request.method() == http::verb::post) {
                return create(parts[2], request);
            }
            if (parts.size() == 5 && parts[0] == "v1" && parts[1] == "workspaces" &&
                parts[3] == "branches") {
                if (request.method() == http::verb::get) return get(parts[2], parts[4]);
                if (request.method() == http::verb::delete_) return close(parts[2], parts[4]);
                if (request.method() == http::verb::patch) return renew(parts[2], parts[4], request);
            }
            if (parts.size() == 6 && parts[0] == "v1" && parts[1] == "workspaces" &&
                parts[3] == "branches" && parts[5] == "attach" &&
                request.method() == http::verb::post) {
                return attach(parts[2], parts[4], request);
            }
            if (parts.size() == 6 && parts[0] == "v1" && parts[1] == "workspaces" &&
                parts[3] == "branches" && parts[5] == "credentials" &&
                request.method() == http::verb::post) {
                return credentials(parts[2], parts[4], request);
            }
            return json_response(http::status::not_found, {{"error", "not found"}});
        } catch (const GatewayError &error) {
            return json_response(http::status::bad_request, {{"error", error.what()}});
        } catch (const std::exception &error) {
            return json_response(http::status::bad_request, {{"error", error.what()}});
        }
    }

    // Versioning routes: each maps to one function of
    // chronos_core.control.operations, run in the embedded interpreter.
    std::optional<http::response<http::string_body>> versioning(
        const std::vector<std::string> &parts,
        const http::request<http::string_body> &request,
        const std::string &query) {
        if (parts.size() < 4 || parts[0] != "v1" || parts[1] != "workspaces") return std::nullopt;
        const auto method = request.method();
        const auto &resource = parts[3];
        std::string operation;
        Json path = Json::object();
        if (resource == "branches" && parts.size() == 4 && method == http::verb::get) {
            operation = "list_branches";
        } else if (resource == "branches" && parts.size() == 6 && parts[5] == "metadata" &&
                   method == http::verb::put) {
            operation = "update_branch_metadata";
            path["branch"] = parts[4];
        } else if (resource == "checkpoints" && parts.size() == 4 && method == http::verb::post) {
            operation = "create_checkpoint";
        } else if (resource == "checkpoints" && parts.size() == 4 && method == http::verb::get) {
            operation = "list_checkpoints";
        } else if (resource == "diff" && parts.size() == 4 && method == http::verb::get) {
            operation = "diff";
        } else if (resource == "merges" && parts.size() == 5 && method == http::verb::post) {
            if (parts[4] == "preview") operation = "merge_preview";
            else if (parts[4] == "apply") operation = "merge_apply";
            else if (parts[4] == "atomic-preview") operation = "merge_atomic_preview";
            else if (parts[4] == "atomic-apply") operation = "merge_atomic";
            else return std::nullopt;
        } else if (resource == "tables" && parts.size() == 4 && method == http::verb::post) {
            operation = "register_table";
        } else if (resource == "tables" && parts.size() == 5 && method == http::verb::get) {
            operation = "table_info";
            path["table"] = parts[4];
        } else if (resource == "tables" && parts.size() == 6 && parts[5] == "indexes" &&
                   method == http::verb::post) {
            operation = "create_index";
            path["table"] = parts[4];
        } else {
            return std::nullopt;
        }
        // Arguments come from the JSON body, then the query, then the path.
        Json arguments = request.body().empty() ? Json::object() : Json::parse(request.body());
        if (!arguments.is_object()) throw GatewayError("request body must be a JSON object");
        arguments.update(query_arguments(query));
        arguments.update(path);
        const auto result = python.call(parts[2], operation, arguments.dump());
        http::response<http::string_body> response(static_cast<http::status>(result.status), 11);
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.body() = result.body;
        response.prepare_payload();
        return response;
    }

    http::response<http::string_body> create(
        const std::string &workspace_name,
        const http::request<http::string_body> &request) {
        const auto body = Json::parse(request.body());
        CreateBranchRequest create;
        create.workspace = workspace_name;
        create.sandbox = body.value("sandbox", true);
        create.from_branch = body.value("from_branch", std::string("main"));
        if (body.contains("from_checkpoint") && !body.at("from_checkpoint").is_null()) {
            create.from_checkpoint = body.at("from_checkpoint").get<std::string>();
        }
        create.branch_id = body.value("branch_id", random_branch_id());
        create.postgres_stores = body.value(
            "databases", std::unordered_set<std::string>{});
        create.filesystem_access = filesystem_access(body);
        create.ttl = ttl(body, create.sandbox
            ? std::optional<std::chrono::seconds>(std::chrono::seconds(3600))
            : std::nullopt);
        create.metadata_json = metadata_json(body);
        return json_response(http::status::created, provisioned_json(runtime.create_branch(create)));
    }

    http::response<http::string_body> credentials(
        const std::string &workspace_name,
        const std::string &branch_id,
        const http::request<http::string_body> &request) {
        const auto body = Json::parse(request.body());
        BranchAccessRequest access;
        access.workspace = workspace_name;
        access.branch_id = branch_id;
        access.postgres_stores = body.value(
            "databases", std::unordered_set<std::string>{});
        access.filesystem_access = filesystem_access(body);
        access.ttl = ttl(body, std::nullopt);
        return json_response(http::status::ok, provisioned_json(runtime.issue_branch_access(access)));
    }

    // The only responses that carry database credentials: branch creation and
    // explicit credential issue.
    Json provisioned_json(const ProvisionedBranch &branch) const {
        if (!branch.access_grant) return branch_json(branch.workspace, branch.branch_id);
        const auto &grant = *branch.access_grant;
        auto result = branch_json(grant);
        Json urls = Json::object();
        for (const auto &store : grant.postgres_stores) {
            urls[store] = "postgresql://" + grant.id + ":" +
                branch.database_credential + "@" +
                postgres.endpoint() + "/" + store;
        }
        result["database_urls"] = std::move(urls);
        return result;
    }

    http::response<http::string_body> attach(
        const std::string &workspace_name,
        const std::string &branch_id,
        const http::request<http::string_body> &request) {
        const auto body = Json::parse(request.body());
        auto grant = runtime.attach_branch(
            workspace_name,
            branch_id,
            body.at("sandbox_id").get<std::string>(),
            body.at("sandbox_ip").get<std::string>());
        auto result = branch_json(grant);
        result["nfs_export"] = nfs.host + ":/" + grant.export_id;
        result["nfs_options"] = "vers=4.2,port=" + std::to_string(nfs.port) +
            ",proto=tcp,hard,nosuid,nodev";
        return json_response(http::status::ok, result);
    }

    http::response<http::string_body> renew(
        const std::string &workspace_name,
        const std::string &branch_id,
        const http::request<http::string_body> &request) {
        const auto body = Json::parse(request.body());
        if (!body.contains("ttl_seconds")) throw GatewayError("ttl_seconds is required");
        auto current = runtime.access_grants().get_for_branch(workspace_name, branch_id);
        if (!current) return json_response(http::status::not_found, {{"error", "not found"}});
        auto grant = runtime.access_grants().renew(current->id, ttl(body, std::nullopt));
        return json_response(http::status::ok, branch_json(grant));
    }

    http::response<http::string_body> get(
        const std::string &workspace_name,
        const std::string &branch_id) {
        auto grant = runtime.access_grants().get_for_branch(workspace_name, branch_id);
        std::optional<chronos::native::NativeBranchInfo> info;
        try {
            info = runtime.branch_info(workspace_name, branch_id);
        } catch (const std::exception &) {
            // A reclaimed sandbox keeps its grant record after its branch is gone.
        }
        if (!grant && !info) return json_response(http::status::not_found, {{"error", "not found"}});
        auto result = grant ? branch_json(*grant) : branch_json(workspace_name, branch_id);
        if (info) {
            result["created_at"] = info->created_at;
            result["metadata"] = Json::parse(info->metadata_json.empty() ? "{}" : info->metadata_json);
            if (!grant) result["sandbox"] = GatewayRuntime::is_sandbox_metadata(info->metadata_json);
        }
        return json_response(http::status::ok, result);
    }

    http::response<http::string_body> close(
        const std::string &workspace_name,
        const std::string &branch_id) {
        auto grant = runtime.access_grants().get_for_branch(workspace_name, branch_id);
        if (grant && grant->owns_branch) {
            // Sandbox: close access now; the reaper deletes the branch later.
            return json_response(
                http::status::ok,
                branch_json(runtime.close_branch(workspace_name, branch_id)));
        }
        runtime.delete_long_lived_branch(workspace_name, branch_id);
        auto result = branch_json(workspace_name, branch_id);
        result["state"] = "deleted";
        return json_response(http::status::ok, result);
    }

    static http::response<http::string_body> json_response(
        http::status status,
        const Json &body) {
        http::response<http::string_body> response(status, 11);
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.body() = body.dump();
        response.prepare_payload();
        return response;
    }

    GatewayRuntime &runtime;
    ListenAddress listen;
    ListenAddress postgres;
    ListenAddress nfs;
    std::string token;
    PythonControl python;
    asio::io_context io;
    tcp::acceptor acceptor;
    std::atomic<bool> stopping{false};
    std::mutex clients_mutex;
    std::vector<std::shared_ptr<tcp::socket>> sockets;
    std::vector<std::thread> clients;
};

ControlServer::ControlServer(
    GatewayRuntime &runtime,
    ListenAddress listen,
    ListenAddress postgres,
    ListenAddress nfs,
    std::string controller_token,
    const std::unordered_map<std::string, WorkspaceConfig> &workspaces)
    : impl_(std::make_unique<Impl>(
          runtime,
          std::move(listen),
          std::move(postgres),
          std::move(nfs),
          std::move(controller_token),
          workspaces)) {}

ControlServer::~ControlServer() = default;
void ControlServer::run() { impl_->run(); }
void ControlServer::stop() { impl_->stop(); }
PythonControl &ControlServer::python_control() { return impl_->python; }

} // namespace chronos::gateway
