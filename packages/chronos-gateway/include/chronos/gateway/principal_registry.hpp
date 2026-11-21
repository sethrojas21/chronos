#pragma once

#include "chronos/gateway/access_grant_registry.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronos::gateway {

// A refused branch or write. The PostgreSQL frontend reports it with SQLSTATE
// 42501 (insufficient_privilege).
class PermissionDenied : public GatewayError {
  public:
    using GatewayError::GatewayError;
};

// The control API reports these as 404 and 409.
class NotFoundError : public GatewayError {
  public:
    using GatewayError::GatewayError;
};

class ConflictError : public GatewayError {
  public:
    using GatewayError::GatewayError;
};

// A stable login of one workspace. Branch patterns are exact branch names,
// "prefix/*" (every branch below prefix/), or "*" (every branch). A branch is
// allowed when it matches `branches` or `read_only_branches`, and writable
// only when it matches `branches` and not `read_only_branches`.
struct Principal {
    std::string workspace;
    std::string name;
    std::string default_branch;
    std::vector<std::string> branches;
    std::vector<std::string> read_only_branches;
    std::int64_t created_at = 0;  // unix seconds
    std::int64_t updated_at = 0;
};

struct PrincipalSpec {
    std::string name;
    std::string default_branch;
    std::vector<std::string> branches;
    std::vector<std::string> read_only_branches;
};

// Fields left empty are kept.
struct PrincipalUpdate {
    std::optional<std::string> default_branch;
    std::optional<std::vector<std::string>> branches;
    std::optional<std::vector<std::string>> read_only_branches;
};

// The password appears only here, when a principal is created or rotated.
struct IssuedPrincipal {
    Principal principal;
    std::string password;
};

enum class BranchAccess {
    None,
    ReadOnly,
    ReadWrite,
};

// What a logged-in connection holds: rotating or deleting the principal
// changes or removes credential_version, which ends the connection.
struct PrincipalLogin {
    Principal principal;
    std::uint64_t credential_version = 0;
};

struct StoredPrincipal {
    Principal principal;
    // "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>"
    std::string password_hash;
};

// Durable storage of one workspace's principals.
class PrincipalStorage {
  public:
    virtual ~PrincipalStorage() = default;
    virtual std::vector<StoredPrincipal> load() = 0;
    virtual void insert(const StoredPrincipal &principal) = 0;
    virtual void update(const StoredPrincipal &principal) = 0;
    virtual void remove(const std::string &name) = 0;
};

class PrincipalRegistry {
  public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    explicit PrincipalRegistry(
        Clock clock = [] { return std::chrono::system_clock::now(); },
        int hash_iterations = kDefaultHashIterations);

    static constexpr int kDefaultHashIterations = 100000;

    // Loads the workspace's principals; later changes are written through.
    void attach_workspace(const std::string &workspace, std::unique_ptr<PrincipalStorage> storage);
    bool has_workspace(const std::string &workspace) const;

    IssuedPrincipal create(const std::string &workspace, const PrincipalSpec &spec);
    IssuedPrincipal rotate(const std::string &workspace, const std::string &name);
    Principal update(const std::string &workspace, const std::string &name, const PrincipalUpdate &update);
    void remove(const std::string &workspace, const std::string &name);
    std::optional<Principal> get(const std::string &workspace, const std::string &name) const;
    std::vector<Principal> list(const std::string &workspace) const;

    // Checks the password; throws GatewayError when it does not match.
    PrincipalLogin authenticate(
        const std::string &workspace,
        const std::string &name,
        const std::string &password) const;
    // The current access of a logged-in connection to a branch. Throws
    // GatewayError when the principal was deleted or its password rotated,
    // and PermissionDenied when the branch is no longer allowed.
    BranchAccess check(
        const std::string &workspace,
        const std::string &name,
        std::uint64_t credential_version,
        const std::string &branch) const;

    static BranchAccess access(const Principal &principal, const std::string &branch);
    static bool pattern_matches(const std::string &pattern, const std::string &branch);
    static void validate_name(const std::string &name);
    static void validate_pattern(const std::string &pattern);
    static std::string hash_password(const std::string &password, int iterations);
    static bool verify_password(const std::string &password, const std::string &encoded);

  private:
    struct Entry {
        StoredPrincipal stored;
        std::uint64_t credential_version = 1;
    };
    struct Workspace {
        std::unique_ptr<PrincipalStorage> storage;
        std::unordered_map<std::string, Entry> principals;
    };

    static void validate(const Principal &principal);
    Workspace &workspace_locked(const std::string &workspace);
    const Workspace &workspace_locked(const std::string &workspace) const;
    std::int64_t now() const;

    const Clock clock_;
    const int hash_iterations_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Workspace> workspaces_;
};

// Stores principals in a gateway-owned table of the workspace's metadata
// database (SQLite or PostgreSQL), through the native SQL driver.
std::unique_ptr<PrincipalStorage> make_sql_principal_storage(
    const std::string &metadata_url,
    const std::string &workspace);

} // namespace chronos::gateway
