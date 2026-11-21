#include "chronos/gateway/runtime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace chronos::gateway {

SqlBranchSession::SqlBranchSession(
    std::unique_ptr<chronos::native::NativeBranchStore> store,
    chronos::native::NativeBranchSession session,
    AccessGrant grant)
    : store_(std::move(store)),
      session_(std::move(session)),
      access_grant_(std::move(grant)) {}

SqlBranchSession::~SqlBranchSession() = default;
SqlBranchSession::SqlBranchSession(SqlBranchSession &&) noexcept = default;
SqlBranchSession &SqlBranchSession::operator=(SqlBranchSession &&) noexcept = default;

GatewayRuntime::GatewayRuntime(
    std::chrono::seconds recovery_window,
    AccessGrantRegistry::Clock clock)
    : access_grants_(recovery_window, std::move(clock)) {}

void GatewayRuntime::add_workspace(const std::string &name, const WorkspaceConfig &config) {
    validate_workspace(name, config);
    auto entry = std::make_shared<Workspace>();
    entry->config = config;

    std::string control_data_url;
    std::string control_metadata_url;
    if (!config.postgres_stores.empty()) {
        control_data_url = config.postgres_stores.begin()->second.data_url;
        control_metadata_url = config.postgres_stores.begin()->second.metadata_url;
    } else if (config.filesystem) {
        control_data_url = config.filesystem->data_url;
        control_metadata_url = config.filesystem->metadata_url;
    }
    if (control_data_url.empty()) throw GatewayError("workspace has no control store");
    entry->control_store = control_metadata_url.empty()
        ? std::make_unique<chronos::native::NativeBranchStore>(control_data_url)
        : std::make_unique<chronos::native::NativeBranchStore>(
              control_data_url, control_metadata_url);
    // Gateway clients may issue branch-local DDL. Migrate tables registered by
    // older callers into the schema-version catalog before serving branches.
    entry->control_store->ensure(config.schema_branching);

    if (config.filesystem) {
        entry->filesystem = std::make_unique<chronos::native::NativeChronosFilesystem>(
            config.filesystem->data_url,
            config.filesystem->metadata_url,
            config.filesystem->block_size);
        entry->filesystem->ensure();
    }

    {
        std::lock_guard lock(workspaces_mutex_);
        if (workspaces_.count(name)) throw GatewayError("workspace is already configured");
    }
    // Principals live in a gateway-owned table of the workspace's metadata
    // database, so they survive a gateway restart.
    principals_.attach_workspace(name, make_sql_principal_storage(control_metadata_url, name));

    std::lock_guard lock(workspaces_mutex_);
    if (!workspaces_.emplace(name, std::move(entry)).second) {
        throw GatewayError("workspace is already configured");
    }
}

std::vector<std::string> GatewayRuntime::postgres_store_aliases(const std::string &workspace_name) const {
    auto target = workspace(workspace_name);
    std::vector<std::string> aliases;
    for (const auto &[alias, store] : target->config.postgres_stores) aliases.push_back(alias);
    std::sort(aliases.begin(), aliases.end());
    return aliases;
}

namespace {

constexpr const char *kSandboxOwner = "chronos-gateway";

void require_known_stores(
    const WorkspaceConfig &config,
    const std::unordered_set<std::string> &stores,
    FilesystemAccess filesystem_access,
    bool has_filesystem) {
    for (const auto &store : stores) {
        if (config.postgres_stores.find(store) == config.postgres_stores.end()) {
            throw GatewayError("branch requests an unknown database");
        }
    }
    if (filesystem_access != FilesystemAccess::None && !has_filesystem) {
        throw GatewayError("workspace has no filesystem");
    }
}

std::string sandbox_metadata(const std::string &metadata_json) {
    auto metadata = nlohmann::json::parse(metadata_json.empty() ? "{}" : metadata_json);
    if (!metadata.is_object()) throw GatewayError("branch metadata must be a JSON object");
    metadata["owner"] = kSandboxOwner;
    return metadata.dump();
}

} // namespace

bool GatewayRuntime::is_sandbox_metadata(const std::string &metadata_json) {
    try {
        const auto metadata = nlohmann::json::parse(metadata_json.empty() ? "{}" : metadata_json);
        return metadata.is_object() && metadata.value("owner", std::string()) == kSandboxOwner;
    } catch (const nlohmann::json::exception &) {
        return false;
    }
}

ProvisionedBranch GatewayRuntime::create_branch(const CreateBranchRequest &request) {
    if (request.from_branch.empty() && !request.from_checkpoint) throw GatewayError("source branch is required");
    if (request.branch_id.empty()) throw GatewayError("branch id is required");
    if (request.sandbox && !request.ttl) throw GatewayError("sandbox branches require a ttl");
    if (request.sandbox && request.from_checkpoint) {
        throw GatewayError("sandbox branches are created from a branch");
    }
    auto target = workspace(request.workspace);
    require_known_stores(
        target->config, request.postgres_stores, request.filesystem_access,
        static_cast<bool>(target->filesystem));
    {
        std::lock_guard lock(target->control_mutex);
        if (request.from_checkpoint) {
            target->control_store->create_branch_from_checkpoint(
                request.branch_id, *request.from_checkpoint);
            const auto metadata = nlohmann::json::parse(request.metadata_json);
            if (!metadata.is_object()) throw GatewayError("branch metadata must be a JSON object");
            if (!metadata.empty()) {
                target->control_store->update_branch_metadata(request.branch_id, request.metadata_json);
            }
        } else {
            target->control_store->create_branch(
                request.branch_id,
                request.from_branch,
                request.sandbox,
                request.sandbox ? sandbox_metadata(request.metadata_json) : request.metadata_json);
        }
        if (target->filesystem) target->filesystem->refresh(request.branch_id);
    }
    const bool wants_access =
        !request.postgres_stores.empty() || request.filesystem_access != FilesystemAccess::None;
    if (!request.sandbox && !wants_access) {
        return ProvisionedBranch{request.workspace, request.branch_id, std::nullopt, {}};
    }
    AccessGrantSpec spec;
    spec.workspace = request.workspace;
    spec.branch = request.branch_id;
    spec.postgres_stores = request.postgres_stores;
    spec.filesystem_access = request.filesystem_access;
    spec.ttl = request.ttl;
    spec.owns_branch = request.sandbox;
    try {
        return grant_access(spec, false);
    } catch (...) {
        delete_branch(*target, request.branch_id);
        throw;
    }
}

ProvisionedBranch GatewayRuntime::issue_branch_access(const BranchAccessRequest &request) {
    auto target = workspace(request.workspace);
    require_known_stores(
        target->config, request.postgres_stores, request.filesystem_access,
        static_cast<bool>(target->filesystem));
    {
        std::lock_guard lock(target->control_mutex);
        const auto info = target->control_store->get_branch(request.branch_id);
        if (is_sandbox_metadata(info.metadata_json)) {
            throw GatewayError("sandbox branch access is issued only when the branch is created");
        }
    }
    AccessGrantSpec spec;
    spec.workspace = request.workspace;
    spec.branch = request.branch_id;
    spec.postgres_stores = request.postgres_stores;
    spec.filesystem_access = request.filesystem_access;
    spec.ttl = request.ttl;
    spec.owns_branch = false;
    return grant_access(spec, true);
}

ProvisionedBranch GatewayRuntime::grant_access(const AccessGrantSpec &spec, bool reissue) {
    auto issued = reissue ? access_grants_.reissue(spec) : access_grants_.create(spec);
    issued.grant = access_grants_.activate(issued.grant.id);
    return ProvisionedBranch{
        spec.workspace, spec.branch, std::move(issued.grant), std::move(issued.credential)};
}

chronos::native::NativeBranchInfo GatewayRuntime::branch_info(
    const std::string &workspace_name,
    const std::string &branch_id) {
    auto target = workspace(workspace_name);
    std::lock_guard lock(target->control_mutex);
    return target->control_store->get_branch(branch_id);
}

void GatewayRuntime::delete_long_lived_branch(
    const std::string &workspace_name,
    const std::string &branch_id) {
    auto target = workspace(workspace_name);
    if (auto grant = access_grants_.get_for_branch(workspace_name, branch_id)) {
        if (grant->owns_branch) throw GatewayError("sandbox branches are closed, then reclaimed");
        access_grants_.revoke(grant->id);
        access_grants_.mark_deleted(grant->id);
    }
    delete_branch(*target, branch_id);
}

AccessGrant GatewayRuntime::attach_branch(
    const std::string &workspace_name,
    const std::string &branch_id,
    const std::string &sandbox_id,
    const std::string &sandbox_ip) {
    auto grant = access_grants_.get_for_branch(workspace_name, branch_id);
    if (!grant) throw GatewayError("workspace branch not found");
    return access_grants_.attach_sandbox(grant->id, sandbox_id, sandbox_ip);
}

AccessGrant GatewayRuntime::close_branch(
    const std::string &workspace_name,
    const std::string &branch_id) {
    auto grant = access_grants_.get_for_branch(workspace_name, branch_id);
    if (!grant) throw GatewayError("workspace branch not found");
    return access_grants_.revoke(grant->id);
}

std::unique_ptr<SqlBranchSession> GatewayRuntime::checkout_postgres(
    const Workspace &target,
    const std::string &store_alias,
    const std::string &branch,
    AccessGrant grant) {
    auto found = target.config.postgres_stores.find(store_alias);
    if (found == target.config.postgres_stores.end()) {
        throw GatewayError("authorized database is not configured");
    }
    auto store = found->second.metadata_url.empty()
        ? std::make_unique<chronos::native::NativeBranchStore>(found->second.data_url)
        : std::make_unique<chronos::native::NativeBranchStore>(
              found->second.data_url, found->second.metadata_url);
    // Read the move count before checking out, so a move that lands in
    // between makes the session stale rather than going unnoticed.
    const auto generation = branch_generation(grant.workspace, branch);
    auto session = store->checkout(branch);
    auto opened = std::make_unique<SqlBranchSession>(
        std::move(store), std::move(session), std::move(grant));
    opened->branch_generation_ = generation;
    opened->set_schema_branching(target.config.schema_branching);
    opened->store_alias_ = store_alias;
    opened->startup_branch_ = branch;
    return opened;
}

std::unique_ptr<SqlBranchSession> GatewayRuntime::open_postgres(
    const std::string &user,
    const std::string &credential,
    const std::string &database,
    const std::optional<std::string> &requested_branch) {
    // Access grants are checked first; principal names never have a grant
    // id's shape, so a user names at most one of the two.
    if (!access_grants_.get(user)) {
        std::string workspace_name;
        std::string store_alias = database;
        if (const auto slash = database.find('/'); slash != std::string::npos) {
            workspace_name = database.substr(0, slash);
            store_alias = database.substr(slash + 1);
        }
        std::vector<std::shared_ptr<Workspace>> candidates;
        std::vector<std::string> candidate_names;
        {
            std::lock_guard lock(workspaces_mutex_);
            for (const auto &[name, entry] : workspaces_) {
                if (!workspace_name.empty() && name != workspace_name) continue;
                if (!entry->config.postgres_stores.count(store_alias)) continue;
                candidates.push_back(entry);
                candidate_names.push_back(name);
            }
        }
        std::optional<std::size_t> chosen;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (!principals_.get(candidate_names[index], user)) continue;
            if (chosen) {
                throw GatewayError(
                    "login name exists in several workspaces; connect to database "
                    "\"workspace/" + store_alias + "\"");
            }
            chosen = index;
        }
        if (chosen) {
            const auto &name = candidate_names[*chosen];
            const auto login = principals_.authenticate(name, user, credential);
            const auto branch = requested_branch ? *requested_branch : login.principal.default_branch;
            const auto access = principals_.check(name, user, login.credential_version, branch);
            AccessGrant grant;
            grant.workspace = name;
            grant.branch = branch;
            grant.postgres_stores = {store_alias};
            grant.state = AccessGrantState::Active;
            grant.owns_branch = false;
            grant.expires_at = std::chrono::system_clock::time_point::max();
            auto opened = checkout_postgres(*candidates[*chosen], store_alias, branch, std::move(grant));
            opened->principal_ = user;
            opened->credential_version_ = login.credential_version;
            opened->read_only_ = access == BranchAccess::ReadOnly;
            return opened;
        }
    }
    auto grant = access_grants_.authorize_postgres(user, credential, database);
    if (requested_branch && *requested_branch != grant.branch) {
        throw PermissionDenied(
            "this access grant is bound to branch \"" + grant.branch + "\"");
    }
    auto target = workspace(grant.workspace);
    const auto branch = grant.branch;
    return checkout_postgres(*target, database, branch, std::move(grant));
}

void GatewayRuntime::recheck_postgres(
    SqlBranchSession &session,
    const std::string &user,
    const std::string &credential,
    const std::string &database) {
    if (!session.principal()) {
        access_grants_.authorize_postgres(user, credential, database);
        return;
    }
    const auto access = principals_.check(
        session.workspace(), *session.principal(), session.credential_version(), session.branch());
    session.set_read_only(access == BranchAccess::ReadOnly);
}

namespace {
std::string branch_generation_key(const std::string &workspace, const std::string &branch) {
    return workspace + '\n' + branch;
}
} // namespace

std::uint64_t GatewayRuntime::branch_generation(const std::string &workspace, const std::string &branch) const {
    std::lock_guard lock(branch_generations_mutex_);
    const auto found = branch_generations_.find(branch_generation_key(workspace, branch));
    return found == branch_generations_.end() ? 0 : found->second;
}

void GatewayRuntime::note_branch_moved(const std::string &workspace, const std::string &branch) {
    std::lock_guard lock(branch_generations_mutex_);
    ++branch_generations_[branch_generation_key(workspace, branch)];
}

std::unique_ptr<SqlBranchSession> GatewayRuntime::refresh_postgres(const SqlBranchSession &current) {
    auto target = workspace(current.workspace());
    auto opened = checkout_postgres(*target, current.store_alias(), current.branch(), current.access_grant());
    opened->principal_ = current.principal_;
    opened->credential_version_ = current.credential_version_;
    opened->startup_branch_ = current.startup_branch_;
    opened->read_only_ = current.read_only_;
    if (!current.search_path().empty()) opened->set_search_path(current.search_path());
    return opened;
}

std::unique_ptr<SqlBranchSession> GatewayRuntime::switch_postgres_branch(
    const SqlBranchSession &current,
    const std::string &branch) {
    if (!current.principal()) {
        throw PermissionDenied(
            "this access grant is bound to branch \"" + current.branch() +
            "\"; log in as a principal to change branches");
    }
    if (branch.empty()) throw GatewayError("branch name is required");
    const auto access = principals_.check(
        current.workspace(), *current.principal(), current.credential_version(), branch);
    auto target = workspace(current.workspace());
    AccessGrant grant = current.access_grant();
    grant.branch = branch;
    auto opened = checkout_postgres(*target, current.store_alias(), branch, std::move(grant));
    opened->principal_ = current.principal_;
    opened->credential_version_ = current.credential_version_;
    opened->startup_branch_ = current.startup_branch_;
    opened->read_only_ = access == BranchAccess::ReadOnly;
    if (!current.search_path().empty()) opened->set_search_path(current.search_path());
    return opened;
}

chronos::native::NativeChronosFilesystem &GatewayRuntime::authorize_filesystem(
    const std::string &export_id,
    const std::string &source_ip,
    bool write,
    AccessGrant *authorized_grant) {
    auto grant = access_grants_.authorize_filesystem(export_id, source_ip, write);
    auto target = workspace(grant.workspace);
    if (!target->filesystem) throw GatewayError("authorized filesystem is not configured");
    if (authorized_grant) *authorized_grant = grant;
    return *target->filesystem;
}

chronos::native::NativeChronosFilesystem &GatewayRuntime::authorize_filesystem_capability(
    const std::string &export_id,
    bool write,
    AccessGrant *authorized_grant) {
    auto grant = access_grants_.authorize_filesystem_capability(export_id, write);
    auto target = workspace(grant.workspace);
    if (!target->filesystem) throw GatewayError("authorized filesystem is not configured");
    if (authorized_grant) *authorized_grant = grant;
    return *target->filesystem;
}

std::vector<AccessGrant> GatewayRuntime::expire_due() {
    return access_grants_.expire_due();
}

std::vector<AccessGrant> GatewayRuntime::reclaim_due() {
    auto due = access_grants_.due_for_reclamation();
    std::vector<AccessGrant> reclaimed;
    reclaimed.reserve(due.size());
    for (const auto &grant : due) {
        try {
            // Only a sandbox grant owns its branch; other grants only carry access.
            if (grant.owns_branch) delete_branch(grant);
            reclaimed.push_back(access_grants_.mark_deleted(grant.id));
        } catch (...) {
            // Leave the grant revoked so the next reaper pass retries safely.
        }
    }
    return reclaimed;
}

void GatewayRuntime::validate_workspace(
    const std::string &name,
    const WorkspaceConfig &config) {
    if (name.empty()) throw GatewayError("workspace name is required");
    if (config.postgres_stores.empty() && !config.filesystem) {
        throw GatewayError("workspace must configure at least one store");
    }
    std::string metadata_url;
    auto check_store = [&](const std::string &data_url, const std::string &candidate) {
        if (data_url.empty()) throw GatewayError("store data url is required");
        if (candidate.empty()) {
            throw GatewayError("sandbox gateway stores require a separate metadata url");
        }
        if (candidate == data_url) {
            throw GatewayError("sandbox gateway data and metadata urls must be separate");
        }
        if (metadata_url.empty()) metadata_url = candidate;
        else if (metadata_url != candidate) {
            throw GatewayError("all workspace stores must share one metadata url");
        }
    };
    for (const auto &[alias, store] : config.postgres_stores) {
        if (alias.empty()) throw GatewayError("database alias is required");
        check_store(store.data_url, store.metadata_url);
    }
    if (config.filesystem) {
        if (config.filesystem->block_size <= 0) throw GatewayError("filesystem block size must be positive");
        check_store(config.filesystem->data_url, config.filesystem->metadata_url);
    }
}

std::shared_ptr<GatewayRuntime::Workspace> GatewayRuntime::workspace(
    const std::string &name) const {
    std::lock_guard lock(workspaces_mutex_);
    auto found = workspaces_.find(name);
    if (found == workspaces_.end()) throw GatewayError("workspace is not configured");
    return found->second;
}

void GatewayRuntime::delete_branch(const AccessGrant &grant) {
    delete_branch(*workspace(grant.workspace), grant.branch);
}

void GatewayRuntime::delete_branch(Workspace &target, const std::string &branch_id) {
    std::lock_guard lock(target.control_mutex);
    if (target.filesystem) target.filesystem->refresh(branch_id);
    target.control_store->delete_branch(branch_id);
}

} // namespace chronos::gateway
