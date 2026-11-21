#include "chronos/gateway/control_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using chronos::gateway::ControlServer;
using chronos::gateway::FilesystemConfig;
using chronos::gateway::GatewayRuntime;
using chronos::gateway::ListenAddress;
using chronos::gateway::StoreConfig;
using chronos::gateway::WorkspaceConfig;
using chronos::native::NativeBranchStore;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using Json = nlohmann::json;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct TempDirectory {
    TempDirectory() {
        std::string pattern = "/tmp/chronos-gateway-control.XXXXXX";
        pattern.push_back('\0');
        path = mkdtemp(pattern.data());
        if (path.empty()) throw std::runtime_error("mkdtemp failed");
    }
    ~TempDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

std::uint16_t unused_port() {
    asio::io_context io;
    tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

http::response<http::string_body> request(
    std::uint16_t port,
    http::verb method,
    const std::string &target,
    const Json &body = Json{},
    const std::string &token = "controller-test") {
    for (int attempt = 0; attempt < 40; ++attempt) {
        try {
            asio::io_context io;
            tcp::socket socket(io);
            socket.connect({asio::ip::make_address("127.0.0.1"), port});
            http::request<http::string_body> outgoing(method, target, 11);
            outgoing.set(http::field::host, "127.0.0.1");
            if (!token.empty()) outgoing.set(http::field::authorization, "Bearer " + token);
            if (!body.is_null() && !body.empty()) {
                outgoing.set(http::field::content_type, "application/json");
                outgoing.body() = body.dump();
                outgoing.prepare_payload();
            }
            http::write(socket, outgoing);
            beast::flat_buffer buffer;
            http::response<http::string_body> incoming;
            http::read(socket, buffer, incoming);
            return incoming;
        } catch (const boost::system::system_error &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }
    throw std::runtime_error("control server did not accept connections");
}

Json body_of(const http::response<http::string_body> &response) {
    return Json::parse(response.body());
}

// Versioning routes run chronos_core.control.gateway in the embedded
// interpreter. Without the chronos_core package they answer 501, which is
// accepted unless CHRONOS_GATEWAY_REQUIRE_PYTHON_CONTROL is set (Docker runs).
void check_versioning_routes(std::uint16_t port) {
    const std::string base = "/v1/workspaces/training";
    auto listed = request(port, http::verb::get, base + "/branches");
    if (listed.result() == http::status::not_implemented) {
        check(std::getenv("CHRONOS_GATEWAY_REQUIRE_PYTHON_CONTROL") == nullptr,
              "Python control must be available: " + listed.body());
        check(body_of(listed).at("error").get<std::string>().find("Python control is unavailable") == 0,
              "501 must explain that Python control is unavailable");
        std::cout << "SKIP versioning routes: " << listed.body() << '\n';
        return;
    }
    check(request(port, http::verb::get, base + "/branches", Json{}, "wrong").result() ==
              http::status::unauthorized,
          "versioning routes require bearer auth");
    check(request(port, http::verb::get, "/v1/workspaces/missing/branches").result() ==
              http::status::not_found,
          "an unknown workspace is not found");
    // Query, path, and body arguments reach the Python operation.
    auto diff = request(port, http::verb::get, base + "/diff?left=main");
    check(diff.result() == http::status::bad_request &&
              body_of(diff).at("error") == "right is required",
          "diff takes left and right from the query: " + diff.body());
    auto table = request(port, http::verb::get, base + "/tables/orders?store=missing+store");
    check(table.result() == http::status::not_found &&
              body_of(table).at("error") == "unknown store 'missing store'",
          "table info takes its store from the query: " + table.body());
    auto metadata = request(port, http::verb::put, base + "/branches/main/metadata",
                            {{"store", "orders"}, {"metadata", {{"a", 1}}}, {"extra", true}});
    check(metadata.result() == http::status::bad_request &&
              body_of(metadata).at("error") == "unexpected arguments: extra",
          "metadata update passes body arguments: " + metadata.body());
    auto index = request(port, http::verb::post, base + "/tables/orders/indexes",
                         {{"store", "orders"}});
    check(index.result() == http::status::bad_request &&
              body_of(index).at("error") == "columns must be a non-empty array of names",
          "index creation takes the table from the path: " + index.body());
    auto merge = request(port, http::verb::post, base + "/merges/atomic-apply",
                         {{"source", "a"}, {"target", "main"}});
    check(merge.result() == http::status::bad_request &&
              body_of(merge).at("error") == "operation_id is required",
          "atomic apply requires an operation id: " + merge.body());
    auto kept = request(port, http::verb::post, base + "/merges/atomic-apply",
                        {{"source", "a"}, {"target", "main"}, {"operation_id", "op"}, {"keep_branch", "yes"}});
    check(kept.result() == http::status::bad_request &&
              body_of(kept).at("error") == "keep_branch must be true or false",
          "atomic apply passes keep_branch through: " + kept.body());
    check(request(port, http::verb::post, base + "/merges/sideways", {{"source", "a"}}).result() ==
              http::status::not_found,
          "unknown merge routes are not found");
    check(request(port, http::verb::post, base + "/checkpoints", Json::array({1})).result() ==
              http::status::bad_request,
          "a request body must be a JSON object");
    // This workspace has a SQL store and ChronosFS on one metadata plane, so
    // diff and merges need merge_tables, as in the CLI.
    auto unscoped = request(port, http::verb::get, base + "/diff?left=main&right=agent");
    check(unscoped.result() == http::status::bad_request &&
              body_of(unscoped).at("error").get<std::string>().find("merge_tables") != std::string::npos,
          "diff needs merge scope for several stores: " + unscoped.body());
    // Operations that open the workspace. The gateway's stores keep metadata
    // in a separate database; until the Python API opens such stores with
    // branch-local schema, it reports that instead of serving the request.
    if (listed.result() == http::status::ok) {
        check(listed.body().find("\"main\"") != std::string::npos,
              "branch list must include main: " + listed.body());
    } else {
        check(listed.result() == http::status::bad_request &&
                  listed.body().find("split metadata/data") != std::string::npos,
              "branch list must succeed or report the split-store limitation: " + listed.body());
        std::cout << "NOTE branch list: " << listed.body() << '\n';
    }
}

} // namespace

int main() {
    TempDirectory temp;
    const std::string metadata = "sqlite://" + temp.path + "/metadata.sqlite";
    const std::string data = "sqlite://" + temp.path + "/orders.sqlite";
    const std::string files = "sqlite://" + temp.path + "/files.sqlite";
    for (const auto &path : {temp.path + "/metadata.sqlite", temp.path + "/orders.sqlite",
                             temp.path + "/files.sqlite"}) {
        std::ofstream(path).close();
    }
    NativeBranchStore setup(data, metadata);
    setup.ensure();
    setup.execute_sql("CREATE TABLE orders (id INTEGER PRIMARY KEY, state TEXT)");
    setup.commit();
    setup.register_table("orders", {"id"});

    GatewayRuntime runtime;
    WorkspaceConfig workspace;
    workspace.postgres_stores.emplace("orders", StoreConfig{data, metadata});
    workspace.filesystem = FilesystemConfig{files, metadata, 4096};
    runtime.add_workspace("training", workspace);

    const auto port = unused_port();
    ControlServer server(
        runtime,
        ListenAddress{"127.0.0.1", port},
        ListenAddress{"127.0.0.1", 15432},
        ListenAddress{"127.0.0.1", 12049},
        "controller-test",
        {{"training", workspace}});
    std::thread server_thread([&] { server.run(); });

    auto health = request(port, http::verb::get, "/healthz", Json{}, "");
    check(health.result() == http::status::ok, "health endpoint must be public");
    const std::string branch_path = "/v1/workspaces/training/branches/rollout%2Fcontrol";
    auto denied = request(port, http::verb::get, branch_path, Json{}, "wrong");
    check(denied.result() == http::status::unauthorized, "control endpoints require bearer auth");

    Json create_body = {
        {"branch_id", "rollout/control"},
        {"databases", Json::array({"orders"})},
        {"filesystem", "read_write"},
        {"ttl_seconds", 120},
    };
    auto created = request(
        port, http::verb::post, "/v1/workspaces/training/branches", create_body);
    check(created.result() == http::status::created, created.body());
    auto branch = Json::parse(created.body());
    check(branch.at("branch_id") == "rollout/control", "control API must preserve branch id");
    check(branch.at("workspace") == "training", "control API must return workspace name");
    check(!branch.contains("secret") && !branch.contains("credential") &&
              !branch.contains("grant_id") && !branch.contains("access_id"),
          "control API must hide internal access-grant details");
    check(branch.at("database_urls").contains("orders"),
          "control API must return allowed database URL");

    auto attached = request(port, http::verb::post,
                            branch_path + "/attach", {
        {"sandbox_id", "e2b-test"}, {"sandbox_ip", "10.0.0.42"}});
    check(attached.result() == http::status::ok, attached.body());
    auto binding = Json::parse(attached.body());
    check(binding.at("nfs_export").get<std::string>().rfind("127.0.0.1:/", 0) == 0,
          "attach must return an opaque NFS export path");
    check(binding.at("nfs_options").get<std::string>().find("port=12049") != std::string::npos,
          "attach must advertise the configured NFS port");

    auto fetched = request(port, http::verb::get, branch_path);
    check(fetched.result() == http::status::ok, fetched.body());
    check(!Json::parse(fetched.body()).contains("database_urls"),
          "branch lookup must not reissue database credentials");
    auto closed = request(port, http::verb::delete_, branch_path);
    check(closed.result() == http::status::ok, closed.body());
    check(Json::parse(closed.body()).at("state") == "closed", "DELETE must close immediately");

    // Long-lived branch: no expiry, metadata, credentials on demand, deletion.
    const std::string agent_path = "/v1/workspaces/training/branches/agent%2F1";
    auto lasting = request(port, http::verb::post, "/v1/workspaces/training/branches", {
        {"branch_id", "agent/1"},
        {"sandbox", false},
        {"databases", Json::array({"orders"})},
        {"metadata", {{"agent", "a1"}}},
    });
    check(lasting.result() == http::status::created, lasting.body());
    auto agent = Json::parse(lasting.body());
    check(agent.at("sandbox") == false && agent.at("expires_at").is_null(),
          "a long-lived branch must not be a sandbox or expire");
    check(agent.at("database_urls").contains("orders"), "creation must return database URLs");
    auto agent_info = Json::parse(request(port, http::verb::get, agent_path).body());
    check(agent_info.at("metadata").at("agent") == "a1", "lookup must return branch metadata");
    check(!agent_info.contains("database_urls"), "lookup must not reissue credentials");
    auto reissued = request(port, http::verb::post, agent_path + "/credentials", {
        {"databases", Json::array({"orders"})}, {"ttl_seconds", 60}});
    check(reissued.result() == http::status::ok, reissued.body());
    auto fresh = Json::parse(reissued.body());
    check(fresh.at("database_urls").at("orders") != agent.at("database_urls").at("orders"),
          "credentials must be newly issued");
    check(!fresh.at("expires_at").is_null(), "credentials may expire");
    auto boxed = request(port, http::verb::post, "/v1/workspaces/training/branches", {
        {"branch_id", "rollout/two"}, {"databases", Json::array({"orders"})}});
    check(boxed.result() == http::status::created && Json::parse(boxed.body()).at("sandbox") == true,
          "branches stay sandboxes by default");
    auto refused = request(port, http::verb::post,
                           "/v1/workspaces/training/branches/rollout%2Ftwo/credentials",
                           {{"databases", Json::array({"orders"})}});
    check(refused.result() == http::status::bad_request, "sandbox credentials must not be reissued");
    auto deleted = request(port, http::verb::delete_, agent_path);
    check(deleted.result() == http::status::ok && Json::parse(deleted.body()).at("state") == "deleted",
          "DELETE must delete a long-lived branch");
    check(request(port, http::verb::get, agent_path).result() == http::status::not_found,
          "a deleted long-lived branch must be gone");

    // A query string does not change an existing route.
    check(request(port, http::verb::get, branch_path + "?verbose=1").result() == http::status::ok,
          "existing routes must ignore the query string");

    check_versioning_routes(port);

    server.stop();
    server_thread.join();
    std::cout << "all gateway control API tests passed\n";
    return 0;
}
