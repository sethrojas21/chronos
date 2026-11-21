#include "chronos/gateway/principal_registry.hpp"

#include "interval_data_plane.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using chronos::gateway::BranchAccess;
using chronos::gateway::ConflictError;
using chronos::gateway::GatewayError;
using chronos::gateway::NotFoundError;
using chronos::gateway::PermissionDenied;
using chronos::gateway::Principal;
using chronos::gateway::PrincipalRegistry;
using chronos::gateway::PrincipalSpec;
using chronos::gateway::PrincipalUpdate;
using chronos::gateway::make_sql_principal_storage;

namespace {

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

template <typename Error = GatewayError, typename Function>
void rejects(Function &&function, const std::string &message) {
    try {
        function();
    } catch (const Error &) {
        return;
    } catch (const std::exception &error) {
        check(false, message + " (wrong error: " + error.what() + ")");
    }
    check(false, message);
}

struct TempDirectory {
    TempDirectory() {
        std::string pattern = "/tmp/chronos-gateway-principals.XXXXXX";
        pattern.push_back('\0');
        path = mkdtemp(pattern.data());
        if (path.empty()) throw std::runtime_error("mkdtemp failed");
    }
    ~TempDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

void test_patterns() {
    using R = PrincipalRegistry;
    check(R::pattern_matches("main", "main"), "exact pattern matches its branch");
    check(!R::pattern_matches("main", "main2"), "exact pattern matches only its branch");
    check(R::pattern_matches("agent/*", "agent/1"), "prefix pattern matches a child");
    check(R::pattern_matches("agent/*", "agent/a/b"), "prefix pattern matches a deeper child");
    check(!R::pattern_matches("agent/*", "agent"), "prefix pattern does not match the prefix itself");
    check(!R::pattern_matches("agent/*", "agent/"), "prefix pattern needs a name after the slash");
    check(!R::pattern_matches("agent/*", "agents/1"), "prefix pattern respects the slash");
    check(R::pattern_matches("*", "anything"), "* matches every branch");
    for (const std::string bad : {"", "a*", "*/x", "a/*/b", "a/**", "/*x", "with space", "a\tb"}) {
        rejects([&] { R::validate_pattern(bad); }, "pattern must be refused: " + bad);
    }
    for (const std::string good : {"main", "agent/*", "*", "team/a/*"}) R::validate_pattern(good);

    Principal p;
    p.branches = {"agent/7", "scratch/*"};
    p.read_only_branches = {"main", "scratch/frozen"};
    check(R::access(p, "agent/7") == BranchAccess::ReadWrite, "allowed branch is writable");
    check(R::access(p, "main") == BranchAccess::ReadOnly, "read-only branch is read-only");
    check(R::access(p, "scratch/x") == BranchAccess::ReadWrite, "prefix branch is writable");
    check(R::access(p, "scratch/frozen") == BranchAccess::ReadOnly, "read-only wins over branches");
    check(R::access(p, "agent/8") == BranchAccess::None, "other branches are denied");
}

void test_names_and_hashes() {
    using R = PrincipalRegistry;
    for (const std::string good : {"agent-7", "a", "_svc", "bot.alpha@team", "A1"}) R::validate_name(good);
    for (const std::string &bad : std::vector<std::string>{"", "7agent", "-x", "has space", "semi;colon",
                                  std::string(64, 'a'), "0123456789abcdef0123456789abcdef",
                                  "abcdefabcdefabcdefabcdefabcdefab"}) {
        rejects([&] { R::validate_name(bad); }, "name must be refused: " + bad);
    }
    const auto first = R::hash_password("secret", 1000);
    const auto second = R::hash_password("secret", 1000);
    check(first != second, "each hash has its own salt");
    check(first.rfind("pbkdf2-sha256$1000$", 0) == 0, "hash records its algorithm and cost");
    check(first.find("secret") == std::string::npos, "hash does not contain the password");
    check(R::verify_password("secret", first) && R::verify_password("secret", second),
          "the password verifies against each hash");
    check(!R::verify_password("Secret", first), "a wrong password does not verify");
    check(!R::verify_password("secret", "garbage") && !R::verify_password("secret", ""),
          "a malformed hash never verifies");
}

PrincipalSpec agent_spec() {
    PrincipalSpec spec;
    spec.name = "agent-7";
    spec.default_branch = "agent/7";
    spec.branches = {"agent/7", "scratch/*"};
    spec.read_only_branches = {"main"};
    return spec;
}

void test_registry(const std::string &metadata_url) {
    PrincipalRegistry registry({[] { return std::chrono::system_clock::now(); }}, 1000);
    registry.attach_workspace("ws", make_sql_principal_storage(metadata_url, "ws"));
    registry.attach_workspace("other", make_sql_principal_storage(metadata_url, "other"));
    rejects([&] { registry.attach_workspace("ws", make_sql_principal_storage(metadata_url, "ws")); },
            "a workspace loads its principals once");
    rejects<NotFoundError>([&] { registry.list("missing"); }, "unknown workspace is not found");

    auto spec = agent_spec();
    auto bad = spec;
    bad.default_branch = "agent/8";
    rejects([&] { registry.create("ws", bad); }, "default branch must be allowed");
    bad = spec;
    bad.default_branch = "scratch/*";
    rejects([&] { registry.create("ws", bad); }, "default branch must not be a pattern");
    bad = spec;
    bad.default_branch = "";
    rejects([&] { registry.create("ws", bad); }, "default branch is required");

    auto issued = registry.create("ws", spec);
    check(issued.password.size() == 64, "password is 32 random bytes in hex");
    check(issued.principal.created_at > 0 && issued.principal.workspace == "ws", "creation fills the record");
    rejects<ConflictError>([&] { registry.create("ws", spec); }, "duplicate names conflict");
    auto elsewhere = registry.create("other", spec);
    check(elsewhere.password != issued.password, "names are per workspace");

    auto login = registry.authenticate("ws", "agent-7", issued.password);
    check(login.principal.default_branch == "agent/7", "login returns the principal");
    rejects([&] { registry.authenticate("ws", "agent-7", elsewhere.password); },
            "another workspace's password does not log in");
    rejects([&] { registry.authenticate("ws", "nobody", issued.password); }, "unknown names do not log in");
    check(registry.check("ws", "agent-7", login.credential_version, "main") == BranchAccess::ReadOnly,
          "check reports read-only access");
    rejects<PermissionDenied>([&] { registry.check("ws", "agent-7", login.credential_version, "agent/8"); },
                              "check denies other branches");

    PrincipalUpdate update;
    update.branches = std::vector<std::string>{"agent/7", "agent/8"};
    auto updated = registry.update("ws", "agent-7", update);
    check(updated.branches.size() == 2 && updated.read_only_branches == std::vector<std::string>{"main"},
          "update replaces only the given fields");
    check(registry.check("ws", "agent-7", login.credential_version, "agent/8") == BranchAccess::ReadWrite,
          "an update applies to open logins");
    PrincipalUpdate orphan;
    orphan.branches = std::vector<std::string>{"agent/8"};
    rejects([&] { registry.update("ws", "agent-7", orphan); }, "update must keep the default branch allowed");
    rejects<NotFoundError>([&] { registry.update("ws", "nobody", update); }, "updating an unknown principal");

    auto rotated = registry.rotate("ws", "agent-7");
    check(rotated.password != issued.password, "rotation issues a new password");
    rejects([&] { registry.authenticate("ws", "agent-7", issued.password); }, "the old password stops working");
    rejects([&] { registry.check("ws", "agent-7", login.credential_version, "agent/7"); },
            "rotation ends logins made with the old password");
    auto relogin = registry.authenticate("ws", "agent-7", rotated.password);

    // A new registry on the same database sees the same principals.
    {
        PrincipalRegistry reloaded({[] { return std::chrono::system_clock::now(); }}, 1000);
        reloaded.attach_workspace("ws", make_sql_principal_storage(metadata_url, "ws"));
        auto listed = reloaded.list("ws");
        check(listed.size() == 1 && listed[0].name == "agent-7", "principals persist");
        check(listed[0].branches == std::vector<std::string>{"agent/7", "agent/8"} &&
                  listed[0].read_only_branches == std::vector<std::string>{"main"} &&
                  listed[0].default_branch == "agent/7",
              "persisted permissions round-trip");
        reloaded.authenticate("ws", "agent-7", rotated.password);
        rejects([&] { reloaded.authenticate("ws", "agent-7", issued.password); },
                "a rotated password stays invalid after a reload");
    }

    registry.remove("ws", "agent-7");
    rejects([&] { registry.check("ws", "agent-7", relogin.credential_version, "agent/7"); },
            "deletion ends open logins");
    rejects<NotFoundError>([&] { registry.remove("ws", "agent-7"); }, "deleting twice is not found");
    check(!registry.get("ws", "agent-7"), "a deleted principal is gone");
    registry.remove("other", "agent-7");
    PrincipalRegistry reloaded({[] { return std::chrono::system_clock::now(); }}, 1000);
    reloaded.attach_workspace("ws", make_sql_principal_storage(metadata_url, "ws"));
    check(reloaded.list("ws").empty(), "deletion persists");
}

} // namespace

int main() {
    test_patterns();
    test_names_and_hashes();
    TempDirectory temp;
    const std::string sqlite_path = temp.path + "/metadata.sqlite";
    std::ofstream(sqlite_path).close();
    test_registry("sqlite://" + sqlite_path);
    if (const char *postgres = std::getenv("CHRONOS_GATEWAY_TEST_POSTGRES_METADATA_URL")) {
        // Start from an empty table so reruns against one database pass.
        chronos::native::NativeSqlConnection(postgres).execute_sql(
            "DROP TABLE IF EXISTS _chronos_gateway_principals");
        test_registry(postgres);
        std::cout << "principal registry: PostgreSQL storage tested\n";
    }
    std::cout << "all principal registry tests passed\n";
    return 0;
}
