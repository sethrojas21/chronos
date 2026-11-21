#pragma once

#include "chronos/gateway/access_grant_registry.hpp"
#include "chronosfs_fuse.hpp"
#include "interval_data_plane.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronos::gateway {

struct StoreConfig {
    std::string data_url;
    std::string metadata_url;
};

struct FilesystemConfig {
    std::string data_url;
    std::string metadata_url;
    std::int64_t block_size = 1024 * 1024;
};

struct WorkspaceConfig {
    std::unordered_map<std::string, StoreConfig> postgres_stores;
    std::optional<FilesystemConfig> filesystem;
    // Branch-local schema: each branch has its own table structure, and SQL
    // clients may run DDL on their branch. When false, table structure is
    // shared by every branch (as in the CLI): tables are created and
    // registered through the control API, and branches can be merged.
    bool schema_branching = true;
};

// This is the protocol-facing form of ChronosWorkspaceContext.create_branch:
// the workspace names the coordinated stores and branch_id names their shared
// Chronos branch. The remaining fields only narrow what a sandbox may access.
struct CreateBranchRequest {
    std::string workspace;
    std::string from_branch;
    // Creates the branch from a checkpoint instead of from_branch (long-lived only).
    std::optional<std::string> from_checkpoint;
    std::string branch_id;
    std::unordered_set<std::string> postgres_stores;
    FilesystemAccess filesystem_access = FilesystemAccess::None;
    // Sandboxes always expire. A long-lived branch's access never expires when
    // ttl is empty; the branch itself is never reclaimed.
    std::optional<std::chrono::seconds> ttl{std::chrono::seconds(3600)};
    // A sandbox is a terminal branch owned by its access grant and deleted when
    // the grant is reclaimed. Otherwise the branch is long-lived: it can be
    // branched from and merged, and its access can be reissued.
    bool sandbox = true;
    std::string metadata_json = "{}";
};

// Access to an existing long-lived branch, replacing any previous access.
struct BranchAccessRequest {
    std::string workspace;
    std::string branch_id;
    std::unordered_set<std::string> postgres_stores;
    FilesystemAccess filesystem_access = FilesystemAccess::None;
    std::optional<std::chrono::seconds> ttl;
};

struct ProvisionedBranch {
    std::string workspace;
    std::string branch_id;
    // Empty for a long-lived branch created without data access.
    std::optional<AccessGrant> access_grant;
    std::string database_credential;
};

class SqlBranchSession {
  public:
    SqlBranchSession(
        std::unique_ptr<chronos::native::NativeBranchStore> store,
        chronos::native::NativeBranchSession session,
        AccessGrant grant);
    ~SqlBranchSession();
    SqlBranchSession(SqlBranchSession &&) noexcept;
    SqlBranchSession &operator=(SqlBranchSession &&) noexcept;
    SqlBranchSession(const SqlBranchSession &) = delete;
    SqlBranchSession &operator=(const SqlBranchSession &) = delete;

    chronos::native::NativeBranchSession &session() { return session_; }
    const AccessGrant &access_grant() const { return access_grant_; }
    // False when the workspace shares one table structure across branches;
    // SQL clients then may not change table structure on a branch.
    bool schema_branching() const { return schema_branching_; }
    // The connection's PostgreSQL search_path ("$user" dropped); empty means
    // the default, where names resolve as written.
    const std::vector<std::string> &search_path() const { return search_path_; }
    void set_search_path(std::vector<std::string> schemas) {
        session_.set_search_path(schemas);
        search_path_ = std::move(schemas);
    }
    void set_schema_branching(bool enabled) { schema_branching_ = enabled; }

  private:
    // The session borrows driver state owned by the store, so destruction order
    // must keep the store alive until after the session is gone.
    std::unique_ptr<chronos::native::NativeBranchStore> store_;
    chronos::native::NativeBranchSession session_;
    AccessGrant access_grant_;
    bool schema_branching_ = true;
    std::vector<std::string> search_path_;
};

class GatewayRuntime {
  public:
    explicit GatewayRuntime(
        std::chrono::seconds recovery_window = std::chrono::minutes(10),
        AccessGrantRegistry::Clock clock = [] { return std::chrono::system_clock::now(); });

    void add_workspace(const std::string &name, const WorkspaceConfig &config);
    ProvisionedBranch create_branch(const CreateBranchRequest &request);
    ProvisionedBranch issue_branch_access(const BranchAccessRequest &request);
    chronos::native::NativeBranchInfo branch_info(
        const std::string &workspace,
        const std::string &branch_id);
    // Deletes a long-lived (or grantless) branch now and retires its access.
    void delete_long_lived_branch(
        const std::string &workspace,
        const std::string &branch_id);
    static bool is_sandbox_metadata(const std::string &metadata_json);
    AccessGrant attach_branch(
        const std::string &workspace,
        const std::string &branch_id,
        const std::string &sandbox_id,
        const std::string &sandbox_ip);
    AccessGrant close_branch(
        const std::string &workspace,
        const std::string &branch_id);
    std::unique_ptr<SqlBranchSession> open_postgres(
        const std::string &access_id,
        const std::string &credential,
        const std::string &store_alias);

    chronos::native::NativeChronosFilesystem &authorize_filesystem(
        const std::string &export_id,
        const std::string &source_ip,
        bool write,
        AccessGrant *authorized_grant = nullptr);
    chronos::native::NativeChronosFilesystem &authorize_filesystem_capability(
        const std::string &export_id,
        bool write,
        AccessGrant *authorized_grant = nullptr);

    std::vector<AccessGrant> expire_due();
    std::vector<AccessGrant> reclaim_due();
    AccessGrantRegistry &access_grants() { return access_grants_; }

  private:
    struct Workspace {
        WorkspaceConfig config;
        std::unique_ptr<chronos::native::NativeBranchStore> control_store;
        std::unique_ptr<chronos::native::NativeChronosFilesystem> filesystem;
        std::mutex control_mutex;
    };

    static void validate_workspace(const std::string &name, const WorkspaceConfig &config);
    std::shared_ptr<Workspace> workspace(const std::string &name) const;
    void delete_branch(const AccessGrant &grant);
    void delete_branch(Workspace &target, const std::string &branch_id);
    ProvisionedBranch grant_access(const AccessGrantSpec &spec, bool reissue);

    AccessGrantRegistry access_grants_;
    mutable std::mutex workspaces_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Workspace>> workspaces_;
};

} // namespace chronos::gateway
