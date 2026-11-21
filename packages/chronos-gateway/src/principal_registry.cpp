#include "chronos/gateway/principal_registry.hpp"

#include "interval_data_plane.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

namespace chronos::gateway {
namespace {

constexpr std::size_t kMaxNameLength = 63;
constexpr std::size_t kMaxPatternLength = 255;
constexpr std::size_t kMaxPatterns = 256;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
constexpr std::size_t kPasswordBytes = 32;

std::string hex(const unsigned char *data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        out << std::setw(2) << static_cast<unsigned int>(data[index]);
    }
    return out.str();
}

std::optional<std::vector<unsigned char>> unhex(const std::string &text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> bytes;
    bytes.reserve(text.size() / 2);
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = value(text[index]);
        const int low = value(text[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

std::vector<unsigned char> random_bytes(std::size_t size) {
    std::vector<unsigned char> bytes(size);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw GatewayError("secure random generation failed");
    }
    return bytes;
}

std::string new_password() {
    const auto bytes = random_bytes(kPasswordBytes);
    return hex(bytes.data(), bytes.size());
}

std::vector<unsigned char> pbkdf2(
    const std::string &password,
    const std::vector<unsigned char> &salt,
    int iterations) {
    std::vector<unsigned char> derived(kHashBytes);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), iterations,
                          EVP_sha256(), static_cast<int>(derived.size()), derived.data()) != 1) {
        throw GatewayError("could not hash password");
    }
    return derived;
}

std::vector<std::string> split(const std::string &text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return parts;
        start = end + 1;
    }
}

bool grant_id_shaped(const std::string &name) {
    // Access grant ids are 32 lowercase hex characters; a principal never
    // looks like one, so a startup user names at most one of the two.
    return name.size() == 32 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

PrincipalRegistry::PrincipalRegistry(Clock clock, int hash_iterations)
    : clock_(std::move(clock)), hash_iterations_(hash_iterations) {
    if (!clock_) throw GatewayError("principal clock is required");
    if (hash_iterations_ < 1) throw GatewayError("hash iterations must be positive");
}

void PrincipalRegistry::attach_workspace(
    const std::string &workspace,
    std::unique_ptr<PrincipalStorage> storage) {
    if (!storage) throw GatewayError("principal storage is required");
    Workspace entry;
    for (auto &stored : storage->load()) {
        stored.principal.workspace = workspace;
        auto name = stored.principal.name;
        entry.principals.emplace(std::move(name), Entry{std::move(stored), 1});
    }
    entry.storage = std::move(storage);
    std::unique_lock lock(mutex_);
    if (!workspaces_.emplace(workspace, std::move(entry)).second) {
        throw GatewayError("workspace principals are already loaded");
    }
}

bool PrincipalRegistry::has_workspace(const std::string &workspace) const {
    std::shared_lock lock(mutex_);
    return workspaces_.find(workspace) != workspaces_.end();
}

IssuedPrincipal PrincipalRegistry::create(const std::string &workspace, const PrincipalSpec &spec) {
    validate_name(spec.name);
    Principal principal;
    principal.workspace = workspace;
    principal.name = spec.name;
    principal.default_branch = spec.default_branch;
    principal.branches = spec.branches;
    principal.read_only_branches = spec.read_only_branches;
    validate(principal);
    const auto password = new_password();
    StoredPrincipal stored{std::move(principal), hash_password(password, hash_iterations_)};
    std::unique_lock lock(mutex_);
    auto &target = workspace_locked(workspace);
    if (target.principals.count(spec.name)) throw ConflictError("principal already exists");
    stored.principal.created_at = stored.principal.updated_at = now();
    target.storage->insert(stored);
    auto inserted = target.principals.emplace(spec.name, Entry{std::move(stored), 1});
    return IssuedPrincipal{inserted.first->second.stored.principal, password};
}

IssuedPrincipal PrincipalRegistry::rotate(const std::string &workspace, const std::string &name) {
    const auto password = new_password();
    const auto hash = hash_password(password, hash_iterations_);
    std::unique_lock lock(mutex_);
    auto &target = workspace_locked(workspace);
    auto found = target.principals.find(name);
    if (found == target.principals.end()) throw NotFoundError("principal not found");
    StoredPrincipal stored = found->second.stored;
    stored.password_hash = hash;
    stored.principal.updated_at = now();
    target.storage->update(stored);
    found->second.stored = std::move(stored);
    ++found->second.credential_version;
    return IssuedPrincipal{found->second.stored.principal, password};
}

Principal PrincipalRegistry::update(
    const std::string &workspace,
    const std::string &name,
    const PrincipalUpdate &update) {
    std::unique_lock lock(mutex_);
    auto &target = workspace_locked(workspace);
    auto found = target.principals.find(name);
    if (found == target.principals.end()) throw NotFoundError("principal not found");
    StoredPrincipal stored = found->second.stored;
    if (update.default_branch) stored.principal.default_branch = *update.default_branch;
    if (update.branches) stored.principal.branches = *update.branches;
    if (update.read_only_branches) stored.principal.read_only_branches = *update.read_only_branches;
    validate(stored.principal);
    stored.principal.updated_at = now();
    target.storage->update(stored);
    found->second.stored = std::move(stored);
    return found->second.stored.principal;
}

void PrincipalRegistry::remove(const std::string &workspace, const std::string &name) {
    std::unique_lock lock(mutex_);
    auto &target = workspace_locked(workspace);
    auto found = target.principals.find(name);
    if (found == target.principals.end()) throw NotFoundError("principal not found");
    target.storage->remove(name);
    target.principals.erase(found);
}

std::optional<Principal> PrincipalRegistry::get(
    const std::string &workspace,
    const std::string &name) const {
    std::shared_lock lock(mutex_);
    const auto &target = workspace_locked(workspace);
    auto found = target.principals.find(name);
    if (found == target.principals.end()) return std::nullopt;
    return found->second.stored.principal;
}

std::vector<Principal> PrincipalRegistry::list(const std::string &workspace) const {
    std::shared_lock lock(mutex_);
    const auto &target = workspace_locked(workspace);
    std::vector<Principal> result;
    result.reserve(target.principals.size());
    for (const auto &[name, entry] : target.principals) result.push_back(entry.stored.principal);
    std::sort(result.begin(), result.end(),
              [](const Principal &a, const Principal &b) { return a.name < b.name; });
    return result;
}

PrincipalLogin PrincipalRegistry::authenticate(
    const std::string &workspace,
    const std::string &name,
    const std::string &password) const {
    PrincipalLogin login;
    std::string hash;
    {
        std::shared_lock lock(mutex_);
        const auto &target = workspace_locked(workspace);
        auto found = target.principals.find(name);
        if (found == target.principals.end()) throw GatewayError("invalid login");
        login = PrincipalLogin{found->second.stored.principal, found->second.credential_version};
        hash = found->second.stored.password_hash;
    }
    // The slow hash runs outside the lock; a rotation meanwhile changes the
    // version, so check() refuses this login on its first statement.
    if (!verify_password(password, hash)) throw GatewayError("invalid login");
    return login;
}

BranchAccess PrincipalRegistry::check(
    const std::string &workspace,
    const std::string &name,
    std::uint64_t credential_version,
    const std::string &branch) const {
    std::shared_lock lock(mutex_);
    const auto &target = workspace_locked(workspace);
    auto found = target.principals.find(name);
    if (found == target.principals.end() || found->second.credential_version != credential_version) {
        throw GatewayError("login is no longer valid");
    }
    const auto result = access(found->second.stored.principal, branch);
    if (result == BranchAccess::None) {
        throw PermissionDenied("permission denied for branch \"" + branch + "\"");
    }
    return result;
}

BranchAccess PrincipalRegistry::access(const Principal &principal, const std::string &branch) {
    auto any = [&](const std::vector<std::string> &patterns) {
        return std::any_of(patterns.begin(), patterns.end(),
                           [&](const std::string &pattern) { return pattern_matches(pattern, branch); });
    };
    const bool read_only = any(principal.read_only_branches);
    if (read_only) return BranchAccess::ReadOnly;
    return any(principal.branches) ? BranchAccess::ReadWrite : BranchAccess::None;
}

bool PrincipalRegistry::pattern_matches(const std::string &pattern, const std::string &branch) {
    if (branch.empty()) return false;
    if (pattern == "*") return true;
    if (pattern.size() >= 2 && pattern.compare(pattern.size() - 2, 2, "/*") == 0) {
        const auto prefix = pattern.substr(0, pattern.size() - 1);  // keeps the '/'
        return branch.size() > prefix.size() && branch.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == branch;
}

void PrincipalRegistry::validate_name(const std::string &name) {
    if (name.empty() || name.size() > kMaxNameLength) {
        throw GatewayError("principal name must have 1 to 63 characters");
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (!std::isalpha(first) && first != '_') {
        throw GatewayError("principal name must start with a letter or underscore");
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == '@')) {
            throw GatewayError("principal name may contain only letters, digits, _ - . @");
        }
    }
    if (grant_id_shaped(name)) {
        throw GatewayError("principal name must not look like an access grant id (32 hex characters)");
    }
}

void PrincipalRegistry::validate_pattern(const std::string &pattern) {
    if (pattern.empty() || pattern.size() > kMaxPatternLength) {
        throw GatewayError("branch pattern must have 1 to 255 characters");
    }
    for (const char c : pattern) {
        const auto u = static_cast<unsigned char>(c);
        if (std::iscntrl(u) || std::isspace(u)) {
            throw GatewayError("branch pattern must not contain spaces or control characters");
        }
    }
    const auto star = pattern.find('*');
    if (star == std::string::npos || pattern == "*") return;
    if (star != pattern.size() - 1 || pattern.size() < 3 || pattern[pattern.size() - 2] != '/') {
        throw GatewayError("branch pattern may use * only as \"*\" or a trailing \"/*\"");
    }
}

void PrincipalRegistry::validate(const Principal &principal) {
    if (principal.branches.size() + principal.read_only_branches.size() > kMaxPatterns) {
        throw GatewayError("too many branch patterns");
    }
    for (const auto &pattern : principal.branches) validate_pattern(pattern);
    for (const auto &pattern : principal.read_only_branches) validate_pattern(pattern);
    if (principal.default_branch.empty()) throw GatewayError("default_branch is required");
    if (principal.default_branch.find('*') != std::string::npos) {
        throw GatewayError("default_branch must be a branch name, not a pattern");
    }
    if (access(principal, principal.default_branch) == BranchAccess::None) {
        throw GatewayError("default_branch must match branches or read_only_branches");
    }
}

std::string PrincipalRegistry::hash_password(const std::string &password, int iterations) {
    const auto salt = random_bytes(kSaltBytes);
    const auto derived = pbkdf2(password, salt, iterations);
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + hex(salt.data(), salt.size()) +
           "$" + hex(derived.data(), derived.size());
}

bool PrincipalRegistry::verify_password(const std::string &password, const std::string &encoded) {
    const auto parts = split(encoded, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2-sha256") return false;
    int iterations = 0;
    try {
        std::size_t consumed = 0;
        iterations = std::stoi(parts[1], &consumed);
        if (consumed != parts[1].size()) return false;
    } catch (const std::exception &) {
        return false;
    }
    if (iterations < 1 || iterations > 10000000) return false;
    const auto salt = unhex(parts[2]);
    const auto expected = unhex(parts[3]);
    if (!salt || !expected || expected->size() != kHashBytes) return false;
    const auto derived = pbkdf2(password, *salt, iterations);
    return CRYPTO_memcmp(derived.data(), expected->data(), kHashBytes) == 0;
}

PrincipalRegistry::Workspace &PrincipalRegistry::workspace_locked(const std::string &workspace) {
    auto found = workspaces_.find(workspace);
    if (found == workspaces_.end()) throw NotFoundError("workspace is not configured");
    return found->second;
}

const PrincipalRegistry::Workspace &PrincipalRegistry::workspace_locked(
    const std::string &workspace) const {
    auto found = workspaces_.find(workspace);
    if (found == workspaces_.end()) throw NotFoundError("workspace is not configured");
    return found->second;
}

std::int64_t PrincipalRegistry::now() const {
    return std::chrono::duration_cast<std::chrono::seconds>(clock_().time_since_epoch()).count();
}

// --- SQL storage ---------------------------------------------------------------

namespace {

using chronos::native::IntervalValue;
using chronos::native::NativeSqlConnection;

constexpr const char *kTable = "_chronos_gateway_principals";

std::string patterns_json(const std::vector<std::string> &patterns) {
    return nlohmann::json(patterns).dump();
}

std::vector<std::string> parse_patterns(const IntervalValue &value) {
    const auto *text = std::get_if<std::string>(&value);
    if (!text) throw GatewayError("stored principal has invalid branch patterns");
    return nlohmann::json::parse(*text).get<std::vector<std::string>>();
}

std::string text_of(const IntervalValue &value) {
    const auto *text = std::get_if<std::string>(&value);
    if (!text) throw GatewayError("stored principal has an invalid text column");
    return *text;
}

std::int64_t integer_of(const IntervalValue &value) {
    if (const auto *integer = std::get_if<std::int64_t>(&value)) return *integer;
    if (const auto *text = std::get_if<std::string>(&value)) return std::stoll(*text);
    throw GatewayError("stored principal has an invalid integer column");
}

class SqlPrincipalStorage final : public PrincipalStorage {
  public:
    SqlPrincipalStorage(const std::string &metadata_url, std::string workspace)
        : connection_(metadata_url), workspace_(std::move(workspace)) {
        run(std::string("CREATE TABLE IF NOT EXISTS ") + kTable + " ("
            "workspace TEXT NOT NULL, "
            "name TEXT NOT NULL, "
            "password_hash TEXT NOT NULL, "
            "default_branch TEXT NOT NULL, "
            "branches TEXT NOT NULL, "
            "read_only_branches TEXT NOT NULL, "
            "created_at BIGINT NOT NULL, "
            "updated_at BIGINT NOT NULL, "
            "PRIMARY KEY (workspace, name))", {});
    }

    std::vector<StoredPrincipal> load() override {
        std::lock_guard lock(mutex_);
        auto rows = connection_.query_sql(
            std::string("SELECT name, password_hash, default_branch, branches, read_only_branches, "
                        "created_at, updated_at FROM ") + kTable + " WHERE workspace = ? ORDER BY name",
            {workspace_});
        connection_.commit();
        std::vector<StoredPrincipal> result;
        result.reserve(rows.size());
        for (const auto &row : rows) {
            if (row.size() != 7) throw GatewayError("stored principal has an unexpected shape");
            StoredPrincipal stored;
            stored.principal.workspace = workspace_;
            stored.principal.name = text_of(row[0]);
            stored.password_hash = text_of(row[1]);
            stored.principal.default_branch = text_of(row[2]);
            stored.principal.branches = parse_patterns(row[3]);
            stored.principal.read_only_branches = parse_patterns(row[4]);
            stored.principal.created_at = integer_of(row[5]);
            stored.principal.updated_at = integer_of(row[6]);
            result.push_back(std::move(stored));
        }
        return result;
    }

    void insert(const StoredPrincipal &stored) override {
        const auto &p = stored.principal;
        run(std::string("INSERT INTO ") + kTable +
                " (workspace, name, password_hash, default_branch, branches, read_only_branches, "
                "created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            {workspace_, p.name, stored.password_hash, p.default_branch, patterns_json(p.branches),
             patterns_json(p.read_only_branches), p.created_at, p.updated_at});
    }

    void update(const StoredPrincipal &stored) override {
        const auto &p = stored.principal;
        run(std::string("UPDATE ") + kTable +
                " SET password_hash = ?, default_branch = ?, branches = ?, read_only_branches = ?, "
                "updated_at = ? WHERE workspace = ? AND name = ?",
            {stored.password_hash, p.default_branch, patterns_json(p.branches),
             patterns_json(p.read_only_branches), p.updated_at, workspace_, p.name});
    }

    void remove(const std::string &name) override {
        run(std::string("DELETE FROM ") + kTable + " WHERE workspace = ? AND name = ?",
            {workspace_, name});
    }

  private:
    void run(const std::string &sql, const std::vector<IntervalValue> &params) {
        std::lock_guard lock(mutex_);
        try {
            connection_.execute_sql(sql, params);
            connection_.commit();
        } catch (...) {
            connection_.rollback();
            throw;
        }
    }

    std::mutex mutex_;
    NativeSqlConnection connection_;
    std::string workspace_;
};

} // namespace

std::unique_ptr<PrincipalStorage> make_sql_principal_storage(
    const std::string &metadata_url,
    const std::string &workspace) {
    return std::make_unique<SqlPrincipalStorage>(metadata_url, workspace);
}

} // namespace chronos::gateway
