#include "chronos/gateway/postgres_server.hpp"

#include <boost/asio.hpp>
#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using chronos::gateway::CreateBranchRequest;
using chronos::gateway::GatewayRuntime;
using chronos::gateway::ListenAddress;
using chronos::gateway::PostgresServer;
using chronos::gateway::StoreConfig;
using chronos::gateway::WorkspaceConfig;
using chronos::native::NativeBranchStore;

namespace {

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct TempDirectory {
    TempDirectory() {
        std::string pattern = "/tmp/chronos-gateway-postgres.XXXXXX";
        pattern.push_back('\0');
        path = mkdtemp(pattern.data());
        if (path.empty()) throw std::runtime_error("mkdtemp failed");
    }
    ~TempDirectory() { std::filesystem::remove_all(path); }
    std::string path;
};

struct ConnectionDeleter {
    void operator()(PGconn *connection) const { if (connection) PQfinish(connection); }
};
using Connection = std::unique_ptr<PGconn, ConnectionDeleter>;

struct ResultDeleter {
    void operator()(PGresult *result) const { if (result) PQclear(result); }
};
using Result = std::unique_ptr<PGresult, ResultDeleter>;

std::uint16_t unused_port() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io, {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

Connection connect(
    std::uint16_t port,
    const std::string &grant,
    const std::string &secret,
    const std::string &database) {
    const std::string connection =
        "host=127.0.0.1 port=" + std::to_string(port) +
        " user=" + grant + " password=" + secret +
        " dbname=" + database + " sslmode=prefer connect_timeout=2";
    for (int attempt = 0; attempt < 40; ++attempt) {
        Connection result(PQconnectdb(connection.c_str()));
        if (PQstatus(result.get()) == CONNECTION_OK) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    Connection failed(PQconnectdb(connection.c_str()));
    return failed;
}

Result execute(PGconn *connection, const std::string &sql) {
    return Result(PQexec(connection, sql.c_str()));
}

Result expect_result(
    PGconn *connection,
    const std::string &sql,
    ExecStatusType expected,
    const std::string &message) {
    auto result = execute(connection, sql);
    check(PQresultStatus(result.get()) == expected,
          message + ": " + PQresultErrorMessage(result.get()));
    return result;
}

void expect_cell(
    PGconn *connection,
    const std::string &sql,
    int row,
    int column,
    const std::string &expected,
    const std::string &message) {
    auto result = expect_result(connection, sql, PGRES_TUPLES_OK, message);
    check(PQntuples(result.get()) > row && PQnfields(result.get()) > column,
          message + ": result shape differs");
    check(!PQgetisnull(result.get(), row, column) &&
              std::string(PQgetvalue(result.get(), row, column)) == expected,
          message + ": result value differs");
}

void expect_command(
    PGconn *connection,
    const std::string &sql,
    const std::string &expected_tag,
    const std::string &message) {
    auto result = expect_result(connection, sql, PGRES_COMMAND_OK, message);
    check(std::string(PQcmdStatus(result.get())) == expected_tag,
          message + ": command tag differs: " + PQcmdStatus(result.get()));
}

Connection connect_once(std::uint16_t port, const std::string &user, const std::string &password,
                        const std::string &database, const std::string &options = "") {
    std::string info = "host=127.0.0.1 port=" + std::to_string(port) + " user=" + user +
        " password=" + password + " dbname='" + database + "' sslmode=disable connect_timeout=2";
    if (!options.empty()) info += " options='" + options + "'";
    return Connection(PQconnectdb(info.c_str()));
}

void expect_sqlstate(PGconn *connection, const std::string &sql, const std::string &code,
                     const std::string &message) {
    auto result = execute(connection, sql);
    const char *state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
    check(PQresultStatus(result.get()) == PGRES_FATAL_ERROR && state && std::string(state) == code,
          message + ": expected SQLSTATE " + code + ", got " + (state ? state : "none") + ": " +
              PQresultErrorMessage(result.get()));
}

// Stable logins: a principal logs in by name and password, lands on its
// default branch (or the startup option chronos.branch), may switch with SET
// chronos.branch, and is refused other branches and writes to read-only ones.
void test_principals(GatewayRuntime &runtime, std::uint16_t port, const std::string &data,
                     const std::string &metadata, const std::string &grant_id,
                     const std::string &grant_secret) {
    for (const std::string branch : {"agent/7", "scratch/a", "other"}) {
        CreateBranchRequest lasting;
        lasting.workspace = "training";
        lasting.from_branch = "main";
        lasting.branch_id = branch;
        lasting.sandbox = false;
        lasting.ttl = std::nullopt;
        runtime.create_branch(lasting);
    }
    chronos::gateway::PrincipalSpec spec;
    spec.name = "agent-7";
    spec.default_branch = "agent/7";
    spec.branches = {"agent/7", "scratch/*"};
    spec.read_only_branches = {"main"};
    auto issued = runtime.principals().create("training", spec);
    const auto &password = issued.password;

    // Login with name and password only lands on the default branch.
    auto agent = connect(port, "agent-7", password, "orders");
    check(PQstatus(agent.get()) == CONNECTION_OK, PQerrorMessage(agent.get()));
    expect_cell(agent.get(), "SHOW chronos.branch", 0, 0, "agent/7", "a principal lands on its default branch");
    expect_command(agent.get(),
                   "INSERT INTO orders (id, customer_id, state, amount, note) VALUES (700, 10, 'agent', 1, NULL)",
                   "INSERT 0 1", "a principal writes its own branch");
    expect_cell(agent.get(), "SELECT count(*) FROM orders WHERE id = 700", 0, 0, "1",
                "the write is visible on the branch");

    // HeavenBase sends only search_path in options: still the default branch.
    {
        auto pathed = connect_once(port, "agent-7", password, "orders", "-c search_path=public");
        check(PQstatus(pathed.get()) == CONNECTION_OK, PQerrorMessage(pathed.get()));
        expect_cell(pathed.get(), "SHOW chronos.branch", 0, 0, "agent/7",
                    "search_path alone keeps the default branch");
        expect_cell(pathed.get(), "SHOW search_path", 0, 0, "public", "search_path still applies");
    }

    // Both settings in options; main is read-only for this principal.
    auto reader = connect_once(port, "agent-7", password, "training/orders",
                               "-c search_path=public -c chronos.branch=main");
    check(PQstatus(reader.get()) == CONNECTION_OK, PQerrorMessage(reader.get()));
    expect_cell(reader.get(), "SHOW chronos.branch", 0, 0, "main", "-c chronos.branch selects the branch");
    expect_cell(reader.get(), "SHOW search_path", 0, 0, "public", "search_path is parsed alongside");
    expect_cell(reader.get(), "SELECT count(*) FROM orders WHERE id = 700", 0, 0, "0",
                "main does not see the agent's write");
    expect_cell(reader.get(), "SELECT state FROM orders WHERE id = 1 AND state <> 'delete me' -- update", 0, 0, "open",
                "reads work on a read-only branch; quoted and commented words are not writes");
    expect_sqlstate(reader.get(), "INSERT INTO orders (id, customer_id, state, amount, note) "
                                  "VALUES (701, 10, 'x', 1, NULL)", "42501", "INSERT on a read-only branch");
    expect_sqlstate(reader.get(), "UPDATE orders SET state = 'x' WHERE id = 1", "42501",
                    "UPDATE on a read-only branch");
    expect_sqlstate(reader.get(), "DELETE FROM orders WHERE id = 1", "42501", "DELETE on a read-only branch");
    expect_sqlstate(reader.get(), "CREATE TABLE ro_probe (id INTEGER PRIMARY KEY)", "42501",
                    "DDL on a read-only branch");
    expect_sqlstate(reader.get(), "WITH gone AS (DELETE FROM orders WHERE id = 1 RETURNING id) SELECT * FROM gone",
                    "42501", "a data-modifying WITH on a read-only branch");
    {
        Result prepared(PQexecParams(reader.get(), "DELETE FROM orders WHERE id = $1", 1, nullptr,
                                     std::array<const char *, 1>{"1"}.data(), nullptr, nullptr, 0));
        const char *state = PQresultErrorField(prepared.get(), PG_DIAG_SQLSTATE);
        check(state && std::string(state) == "42501", "extended-protocol writes on a read-only branch are refused");
    }
    expect_cell(reader.get(), "SELECT state FROM orders WHERE id = 1", 0, 0, "open",
                "a refused write changes nothing");

    // A branch outside the principal's patterns is refused at login.
    {
        auto denied = connect_once(port, "agent-7", password, "orders", "-c chronos.branch=other");
        check(PQstatus(denied.get()) == CONNECTION_BAD &&
                  std::string(PQerrorMessage(denied.get())).find("permission denied for branch") != std::string::npos,
              std::string("a denied startup branch must fail: ") + PQerrorMessage(denied.get()));
        auto wrong = connect_once(port, "agent-7", "wrong", "orders");
        check(PQstatus(wrong.get()) == CONNECTION_BAD, "a wrong password must fail");
        auto nobody = connect_once(port, "nobody", password, "orders");
        check(PQstatus(nobody.get()) == CONNECTION_BAD, "an unknown name must fail");
    }

    // SET chronos.branch switches outside a transaction only.
    expect_command(agent.get(), "SET chronos.branch = 'scratch/a'", "SET", "SET switches to an allowed branch");
    expect_cell(agent.get(), "SHOW chronos.branch", 0, 0, "scratch/a", "SHOW reports the new branch");
    expect_cell(agent.get(), "SELECT count(*) FROM orders WHERE id = 700", 0, 0, "0",
                "the new branch has its own data");
    expect_sqlstate(agent.get(), "SET chronos.branch = 'other'", "42501", "SET to a denied branch");
    expect_cell(agent.get(), "SHOW chronos.branch", 0, 0, "scratch/a", "a denied SET keeps the branch");
    expect_command(agent.get(), "BEGIN", "BEGIN", "a transaction begins");
    expect_sqlstate(agent.get(), "SET chronos.branch = 'agent/7'", "25001", "SET inside a transaction");
    expect_command(agent.get(), "ROLLBACK", "ROLLBACK", "the failed transaction rolls back");
    expect_cell(agent.get(), "SHOW chronos.branch", 0, 0, "scratch/a", "SET inside a transaction changes nothing");
    expect_command(agent.get(), "SET chronos.branch TO main", "SET", "SET TO an unquoted name");
    expect_sqlstate(agent.get(), "DELETE FROM orders WHERE id = 1", "42501", "a switched read-only branch refuses writes");
    expect_command(agent.get(), "RESET chronos.branch", "RESET", "RESET returns to the login branch");
    expect_cell(agent.get(), "SELECT count(*) FROM orders WHERE id = 700", 0, 0, "1",
                "RESET returns to the default branch's data");

    // A grant connection is bound to its branch.
    {
        auto granted = connect_once(port, grant_id, grant_secret, "orders", "-c chronos.branch=rollout-wire");
        check(PQstatus(granted.get()) == CONNECTION_OK, PQerrorMessage(granted.get()));
        expect_cell(granted.get(), "SHOW chronos.branch", 0, 0, "rollout-wire", "a grant reports its branch");
        expect_sqlstate(granted.get(), "SET chronos.branch = 'main'", "42501", "a grant cannot switch branches");
        auto elsewhere = connect_once(port, grant_id, grant_secret, "orders", "-c chronos.branch=main");
        check(PQstatus(elsewhere.get()) == CONNECTION_BAD, "a grant cannot log in to another branch");
    }

    // Narrowing permissions applies to open connections.
    auto scratch = connect_once(port, "agent-7", password, "orders", "-c chronos.branch=scratch/a");
    check(PQstatus(scratch.get()) == CONNECTION_OK, PQerrorMessage(scratch.get()));
    chronos::gateway::PrincipalUpdate narrow;
    narrow.branches = std::vector<std::string>{"agent/7"};
    runtime.principals().update("training", "agent-7", narrow);
    expect_sqlstate(scratch.get(), "SELECT 1", "42501", "a removed branch is refused on the next statement");

    // Rotation: the old password stops working, also for open connections.
    auto rotated = runtime.principals().rotate("training", "agent-7");
    auto old_login = connect_once(port, "agent-7", password, "orders");
    check(PQstatus(old_login.get()) == CONNECTION_BAD, "the old password must fail after rotation");
    check(PQresultStatus(execute(agent.get(), "SELECT 1").get()) == PGRES_FATAL_ERROR,
          "rotation ends connections made with the old password");
    auto fresh = connect_once(port, "agent-7", rotated.password, "orders");
    check(PQstatus(fresh.get()) == CONNECTION_OK, PQerrorMessage(fresh.get()));
    expect_cell(fresh.get(), "SHOW chronos.branch", 0, 0, "agent/7", "the new password logs in");

    // A restarted gateway (a new runtime on the same metadata) keeps principals.
    {
        GatewayRuntime restarted;
        WorkspaceConfig workspace;
        workspace.postgres_stores.emplace("orders", StoreConfig{data, metadata});
        restarted.add_workspace("training", workspace);
        const auto restarted_port = unused_port();
        PostgresServer restarted_server(restarted, ListenAddress{"127.0.0.1", restarted_port});
        std::thread restarted_thread([&] { restarted_server.run(); });
        auto again = connect(restarted_port, "agent-7", rotated.password, "orders");
        check(PQstatus(again.get()) == CONNECTION_OK,
              std::string("a principal survives a restart: ") + PQerrorMessage(again.get()));
        expect_cell(again.get(), "SHOW chronos.branch", 0, 0, "agent/7", "the default branch survives a restart");
        expect_cell(again.get(), "SELECT count(*) FROM orders WHERE id = 700", 0, 0, "1",
                    "the restarted login reads its branch");
        auto stale = connect_once(restarted_port, "agent-7", password, "orders");
        check(PQstatus(stale.get()) == CONNECTION_BAD, "a rotated password stays invalid after a restart");
        again.reset();
        stale.reset();
        restarted_server.stop();
        restarted_thread.join();
    }

    runtime.principals().remove("training", "agent-7");
    check(PQresultStatus(execute(fresh.get(), "SELECT 1").get()) == PGRES_FATAL_ERROR,
          "deleting a principal ends its connections");
    auto deleted = connect_once(port, "agent-7", rotated.password, "orders");
    check(PQstatus(deleted.get()) == CONNECTION_BAD, "a deleted principal cannot log in");
    check(PQresultStatus(execute(reader.get(), "SELECT 1").get()) == PGRES_FATAL_ERROR,
          "deleting a principal ends every connection it opened");
}

} // namespace

// A branch reset moves the branch's head in place. A connection that was
// idle on that branch must read the reset state on its next statement, not
// the state it opened on.
void test_reset_reopens_idle_sessions(GatewayRuntime &runtime, std::uint16_t port,
                                      const std::string &data, const std::string &metadata) {
    CreateBranchRequest lasting;
    lasting.workspace = "training";
    lasting.from_branch = "main";
    lasting.branch_id = "reset/1";
    lasting.sandbox = false;
    lasting.ttl = std::nullopt;
    runtime.create_branch(lasting);
    chronos::gateway::PrincipalSpec spec;
    spec.name = "resetter";
    spec.default_branch = "reset/1";
    spec.branches = {"reset/1"};
    auto issued = runtime.principals().create("training", spec);

    auto agent = connect(port, "resetter", issued.password, "orders");
    check(PQstatus(agent.get()) == CONNECTION_OK, PQerrorMessage(agent.get()));
    expect_command(agent.get(),
                   "INSERT INTO orders (id, customer_id, state, amount, note) VALUES (800, 10, 'kept', 1, NULL)",
                   "INSERT 0 1", "a write before the checkpoint");
    NativeBranchStore store(data, metadata);
    store.create_checkpoint("reset-cp", "reset/1");
    expect_command(agent.get(), "UPDATE orders SET state = 'later' WHERE id = 800", "UPDATE 1",
                   "a write after the checkpoint");
    expect_cell(agent.get(), "SELECT state FROM orders WHERE id = 800", 0, 0, "later", "the later write");

    // Reset the branch while the connection stays open, as the reset route does.
    store.reset_branch("reset/1", "reset-cp");
    runtime.note_branch_moved("training", "reset/1");
    expect_cell(agent.get(), "SELECT state FROM orders WHERE id = 800", 0, 0, "kept",
                "an open connection reads the reset state after a reset");
    expect_command(agent.get(), "UPDATE orders SET state = 'after reset' WHERE id = 800", "UPDATE 1",
                   "writes continue on the reset branch");
    {
        auto fresh = connect(port, "resetter", issued.password, "orders");
        expect_cell(fresh.get(), "SELECT state FROM orders WHERE id = 800", 0, 0, "after reset",
                    "a new connection sees the same branch state");
    }
}

int main() {
    TempDirectory temp;
    const char *postgres_data = std::getenv("CHRONOS_GATEWAY_TEST_POSTGRES_DATA_URL");
    const char *postgres_metadata = std::getenv("CHRONOS_GATEWAY_TEST_POSTGRES_METADATA_URL");
    check((postgres_data == nullptr) == (postgres_metadata == nullptr),
          "both PostgreSQL test URLs must be set together");
    const bool use_postgres = postgres_data != nullptr;
    const std::string metadata = use_postgres
        ? postgres_metadata
        : "sqlite://" + temp.path + "/metadata.sqlite";
    const std::string data = use_postgres
        ? postgres_data
        : "sqlite://" + temp.path + "/orders.sqlite";
    if (!use_postgres) {
        std::ofstream(temp.path + "/metadata.sqlite").close();
        std::ofstream(temp.path + "/orders.sqlite").close();
    }

    NativeBranchStore setup(data, metadata);
    setup.ensure();
    setup.execute_sql(
        "CREATE TABLE orders ("
        "id INTEGER PRIMARY KEY, customer_id INTEGER, state TEXT, amount INTEGER, note TEXT)");
    setup.execute_sql("CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT)");
    setup.execute_sql("INSERT INTO customers VALUES (?, ?)",
                      {std::int64_t(10), std::string("Ada")});
    setup.execute_sql("INSERT INTO customers VALUES (?, ?)",
                      {std::int64_t(20), std::string("Linus")});
    setup.execute_sql("INSERT INTO orders VALUES (?, ?, ?, ?, ?)",
                      {std::int64_t(1), std::int64_t(10), std::string("open"),
                       std::int64_t(25), std::monostate{}});
    setup.execute_sql("INSERT INTO orders VALUES (?, ?, ?, ?, ?)",
                      {std::int64_t(2), std::int64_t(10), std::string("open"),
                       std::int64_t(75), std::string("fragile")});
    setup.execute_sql("INSERT INTO orders VALUES (?, ?, ?, ?, ?)",
                      {std::int64_t(3), std::int64_t(20), std::string("closed"),
                       std::int64_t(40), std::string("delivered")});
    setup.commit();
    setup.register_table("orders", {"id"});
    setup.register_table("customers", {"id"});

    GatewayRuntime runtime;
    WorkspaceConfig workspace;
    workspace.postgres_stores.emplace("orders", StoreConfig{data, metadata});
    runtime.add_workspace("training", workspace);
    CreateBranchRequest request;
    request.workspace = "training";
    request.from_branch = "main";
    request.branch_id = "rollout-wire";
    request.postgres_stores = {"orders"};
    auto issued = runtime.create_branch(request);

    const auto port = unused_port();
    PostgresServer server(runtime, ListenAddress{"127.0.0.1", port});
    std::thread server_thread([&] { server.run(); });

    auto connection = connect(port, issued.access_grant->id, issued.database_credential, "orders");
    check(PQstatus(connection.get()) == CONNECTION_OK, PQerrorMessage(connection.get()));
    auto selected = execute(connection.get(), "SELECT state FROM orders WHERE id = 1");
    check(PQresultStatus(selected.get()) == PGRES_TUPLES_OK, PQerrorMessage(connection.get()));
    check(PQntuples(selected.get()) == 1 && std::string(PQgetvalue(selected.get(), 0, 0)) == "open",
          "simple-query SELECT must return branch data");

    auto physical = execute(connection.get(), "SELECT * FROM _chronos_b_interval_orders");
    check(PQresultStatus(physical.get()) == PGRES_FATAL_ERROR,
          "sandbox SQL must not read physical interval tables");
    auto reserved_ddl = execute(connection.get(),
        "CREATE TABLE _chronos_branch_interval_branches (id INTEGER PRIMARY KEY)");
    check(PQresultStatus(reserved_ddl.get()) == PGRES_FATAL_ERROR,
          "sandbox SQL must not create objects in the reserved namespace");

    const char *parameter_values[] = {"1"};
    Result parameterized(PQexecParams(
        connection.get(),
        "SELECT state FROM orders WHERE id = $1",
        1,
        nullptr,
        parameter_values,
        nullptr,
        nullptr,
        0));
    check(PQresultStatus(parameterized.get()) == PGRES_TUPLES_OK,
          PQerrorMessage(connection.get()));
    check(PQntuples(parameterized.get()) == 1 &&
              std::string(PQgetvalue(parameterized.get(), 0, 0)) == "open",
          "extended-query parameters must be executed on the branch");

    // Read compatibility: these all travel over the gateway's pgwire socket
    // and exercise the interval-query rewriter rather than a direct store API.
    auto joined = expect_result(connection.get(),
        "SELECT o.id, c.name, o.amount FROM orders AS o "
        "JOIN customers AS c ON c.id = o.customer_id "
        "WHERE o.state = 'open' ORDER BY o.amount DESC",
        PGRES_TUPLES_OK, "joins, aliases, predicates, and ordering must work");
    check(PQntuples(joined.get()) == 2 &&
              std::string(PQgetvalue(joined.get(), 0, 0)) == "2" &&
              std::string(PQgetvalue(joined.get(), 0, 1)) == "Ada" &&
              std::string(PQgetvalue(joined.get(), 1, 0)) == "1",
          "join result must retain PostgreSQL row order and values");

    auto grouped = expect_result(connection.get(),
        "SELECT customer_id, COUNT(*) AS count, SUM(amount) AS total "
        "FROM orders GROUP BY customer_id HAVING COUNT(*) >= 1 ORDER BY customer_id",
        PGRES_TUPLES_OK, "aggregates, GROUP BY, and HAVING must work");
    check(PQntuples(grouped.get()) == 2 &&
              std::string(PQgetvalue(grouped.get(), 0, 1)) == "2" &&
              std::string(PQgetvalue(grouped.get(), 0, 2)) == "100",
          "aggregate result must be correct");

    auto cte = expect_result(connection.get(),
        "WITH open_orders AS ("
        "SELECT id, customer_id, amount FROM orders WHERE state = 'open') "
        "SELECT customer_id, MAX(amount) FROM open_orders "
        "GROUP BY customer_id ORDER BY customer_id",
        PGRES_TUPLES_OK, "read-only CTEs must work");
    check(PQntuples(cte.get()) == 1 &&
              std::string(PQgetvalue(cte.get(), 0, 1)) == "75",
          "CTE result must be correct");

    expect_cell(connection.get(),
        "SELECT name FROM customers WHERE EXISTS ("
        "SELECT 1 FROM orders WHERE orders.customer_id = customers.id "
        "AND amount > 50)",
        0, 0, "Ada", "correlated EXISTS subqueries must work");
    expect_cell(connection.get(),
        "SELECT CASE WHEN amount >= 50 THEN 'large' ELSE 'small' END "
        "FROM orders WHERE id = 2",
        0, 0, "large", "CASE expressions must work");
    expect_cell(connection.get(),
        "SELECT COALESCE(note, 'none') FROM orders WHERE id = 1",
        0, 0, "none", "NULL and COALESCE must work");

    auto unioned = expect_result(connection.get(),
        "SELECT id FROM orders WHERE id = 1 UNION ALL "
        "SELECT id FROM orders WHERE id = 3 ORDER BY id",
        PGRES_TUPLES_OK, "set operations must work");
    check(PQntuples(unioned.get()) == 2 &&
              std::string(PQgetvalue(unioned.get(), 1, 0)) == "3",
          "UNION ALL result must be correct");

    const char *range_values[] = {"20", "80"};
    Result ranged(PQexecParams(
        connection.get(),
        "SELECT id FROM orders WHERE amount BETWEEN $1 AND $2 ORDER BY id",
        2, nullptr, range_values, nullptr, nullptr, 0));
    check(PQresultStatus(ranged.get()) == PGRES_TUPLES_OK &&
              PQntuples(ranged.get()) == 3,
          "multiple extended-query parameters must work");

    Result prepared(PQprepare(connection.get(), "order_by_id",
        "SELECT state, amount FROM orders WHERE id = $1", 1, nullptr));
    check(PQresultStatus(prepared.get()) == PGRES_COMMAND_OK,
          "named prepared statements must parse");
    const char *prepared_values[] = {"3"};
    Result prepared_result(PQexecPrepared(
        connection.get(), "order_by_id", 1, prepared_values, nullptr, nullptr, 0));
    check(PQresultStatus(prepared_result.get()) == PGRES_TUPLES_OK &&
              PQntuples(prepared_result.get()) == 1 &&
              std::string(PQgetvalue(prepared_result.get(), 0, 0)) == "closed",
          "named prepared statements must bind and execute");
    const char *update_values[] = {"queued", "2"};
    Result parameterized_update(PQexecParams(
        connection.get(),
        "UPDATE orders SET state = $1 WHERE id = $2",
        2, nullptr, update_values, nullptr, nullptr, 0));
    check(PQresultStatus(parameterized_update.get()) == PGRES_COMMAND_OK &&
              std::string(PQcmdStatus(parameterized_update.get())) == "UPDATE 1",
          "extended-query DML must bind and report affected rows");
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 2",
                0, 0, "queued", "extended-query DML must change branch data");

    expect_cell(connection.get(),
        "/* ORM trace: rollout */ -- leading comments are legal\n"
        "SELECT state FROM orders WHERE id = 1",
        0, 0, "open", "leading block and line comments must be accepted");
    auto empty_rows = expect_result(connection.get(),
        "SELECT id FROM orders WHERE id = -1",
        PGRES_TUPLES_OK, "zero-row SELECT must work");
    check(PQntuples(empty_rows.get()) == 0 && PQnfields(empty_rows.get()) == 1,
          "zero-row SELECT must retain its row description");

    const char *null_values[] = {nullptr};
    Result null_parameter(PQexecParams(
        connection.get(), "SELECT $1", 1, nullptr, null_values, nullptr, nullptr, 0));
    check(PQresultStatus(null_parameter.get()) == PGRES_TUPLES_OK &&
              PQntuples(null_parameter.get()) == 1 &&
              PQgetisnull(null_parameter.get(), 0, 0),
          "NULL extended-query parameters must retain SQL NULL");
    expect_command(connection.get(), "SET application_name = 'chronos-sql-test'",
                   "SET", "ordinary session SET commands must be accepted");
    expect_cell(connection.get(), "SHOW standard_conforming_strings",
                0, 0, "on", "SHOW commands required by clients must respond");
    auto empty_query = execute(connection.get(), "");
    check(PQresultStatus(empty_query.get()) == PGRES_EMPTY_QUERY,
          "an empty simple query must return EmptyQueryResponse");

    if (use_postgres) {
        expect_cell(connection.get(), "SELECT DATE '2026-09-15'",
                    0, 0, "2026-09-15", "PostgreSQL date literals must work");
        expect_cell(connection.get(), "SELECT ARRAY[1, 2, 3]::text",
                    0, 0, "{1,2,3}", "PostgreSQL array expressions must work");
        expect_cell(connection.get(), "SELECT '{\"agent\": \"ready\"}'::jsonb::text",
                    0, 0, "{\"agent\": \"ready\"}",
                    "PostgreSQL JSONB expressions must work");
        expect_cell(connection.get(), "SELECT 'branch-世界'::text",
                    0, 0, "branch-世界", "UTF-8 query results must work");
    }

    // DML compatibility and command tags.
    expect_command(connection.get(),
        "INSERT INTO orders (id, customer_id, state, amount, note) VALUES "
        "(4, 20, 'open', 15, NULL), (5, 10, 'open', 60, 'gift')",
        "INSERT 0 2", "multi-row INSERT must work");
    expect_command(connection.get(),
        "INSERT INTO orders (id, customer_id, state, amount, note) "
        "VALUES (5, 10, 'duplicate', 1, NULL) ON CONFLICT DO NOTHING",
        "INSERT 0 0", "ON CONFLICT DO NOTHING must work");
    expect_command(connection.get(),
        "UPDATE orders SET amount = amount + 5, note = COALESCE(note, 'updated') "
        "WHERE customer_id = 20",
        "UPDATE 2", "set-based UPDATE expressions must work");
    expect_cell(connection.get(), "SELECT amount FROM orders WHERE id = 3",
                0, 0, "45", "UPDATE result must be visible");
    expect_command(connection.get(), "DELETE FROM orders WHERE id IN (4, 5)",
                   "DELETE 2", "DELETE predicates must work");
    expect_cell(connection.get(), "SELECT COUNT(*) FROM orders",
                0, 0, "3", "DELETE result must be visible");

    // INSERT ... ON CONFLICT (primary key) DO UPDATE: a new key inserts; an
    // existing key takes the assignments from EXCLUDED and its current values,
    // and keeps the columns the statement does not assign.
    expect_command(connection.get(),
        "INSERT INTO orders (id, customer_id, state, amount, note) VALUES (6, 30, 'open', 10, 'new') "
        "ON CONFLICT (id) DO UPDATE SET state = excluded.state",
        "INSERT 0 1", "ON CONFLICT DO UPDATE must insert a new key");
    expect_command(connection.get(),
        "INSERT INTO orders AS o (id, customer_id, state, amount) VALUES (6, 31, 'paid', 5), (7, 32, 'open', 1) "
        "ON CONFLICT (id) DO UPDATE SET state = EXCLUDED.state, amount = o.amount + excluded.amount",
        "INSERT 0 2", "ON CONFLICT DO UPDATE must insert and update in one statement");
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 6",
                0, 0, "paid", "ON CONFLICT DO UPDATE must apply EXCLUDED values");
    expect_cell(connection.get(), "SELECT amount FROM orders WHERE id = 6",
                0, 0, "15", "ON CONFLICT DO UPDATE must combine current and EXCLUDED values");
    expect_cell(connection.get(), "SELECT customer_id FROM orders WHERE id = 6",
                0, 0, "30", "ON CONFLICT DO UPDATE must keep unassigned columns");
    expect_cell(connection.get(), "SELECT note FROM orders WHERE id = 6",
                0, 0, "new", "ON CONFLICT DO UPDATE must keep columns not in the INSERT");
    {
        const char *values[] = {"7", "32", "shipped", "1"};
        Result upserted(PQexecParams(
            connection.get(),
            "INSERT INTO orders (id, customer_id, state, amount) VALUES ($1, $2, $3, $4) "
            "ON CONFLICT (id) DO UPDATE SET state = excluded.state",
            4, nullptr, values, nullptr, nullptr, 0));
        check(PQresultStatus(upserted.get()) == PGRES_COMMAND_OK,
              std::string("parameterized ON CONFLICT DO UPDATE must work: ") +
                  PQresultErrorMessage(upserted.get()));
        check(std::string(PQcmdStatus(upserted.get())) == "INSERT 0 1",
              "parameterized ON CONFLICT DO UPDATE must report one row");
    }
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 7",
                0, 0, "shipped", "parameterized ON CONFLICT DO UPDATE must update");
    for (const auto *refused : {
             "INSERT INTO orders (id, state) VALUES (6, 'x') ON CONFLICT (customer_id) DO UPDATE SET state = excluded.state",
             "INSERT INTO orders (id, state) VALUES (6, 'x') ON CONFLICT (id) DO UPDATE SET state = excluded.state WHERE orders.amount > 0",
             "INSERT INTO orders (id, state) VALUES (6, 'x'), (6, 'y') ON CONFLICT (id) DO UPDATE SET state = excluded.state",
             "INSERT INTO orders (id, state) VALUES (6, 'x') ON CONFLICT (id) DO UPDATE SET id = excluded.id",
             "INSERT INTO orders (id, state) VALUES (6, 'x') ON CONFLICT DO UPDATE SET state = excluded.state"}) {
        expect_result(connection.get(), refused, PGRES_FATAL_ERROR,
                      std::string("unsupported ON CONFLICT DO UPDATE must fail clearly: ") + refused);
    }
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 6",
                0, 0, "paid", "a refused ON CONFLICT DO UPDATE must not write");
    expect_command(connection.get(), "DELETE FROM orders WHERE id IN (6, 7)",
                   "DELETE 2", "ON CONFLICT DO UPDATE rows must be deletable");

    // Branch-local DDL must be usable immediately through the same pgwire
    // session, including the copied schema and its logical indexes.
    expect_command(connection.get(),
        "CREATE TABLE tasks (id INTEGER PRIMARY KEY, title TEXT)",
        "CREATE 0", "branch-local CREATE TABLE must work");
    expect_command(connection.get(),
        "INSERT INTO tasks (id, title) VALUES (1, 'ship')",
        "INSERT 0 1", "new branch-local tables must accept writes");
    expect_command(connection.get(),
        "ALTER TABLE tasks ADD COLUMN priority TEXT DEFAULT 'normal'",
        "ALTER 0", "branch-local ALTER TABLE ADD COLUMN must work");
    expect_cell(connection.get(), "SELECT priority FROM tasks WHERE id = 1",
                0, 0, "normal", "ALTER TABLE defaults must be visible");
    expect_command(connection.get(), "CREATE INDEX tasks_title_idx ON tasks (title)",
                   "CREATE 0", "branch-local CREATE INDEX must work");
    expect_command(connection.get(), "DROP INDEX tasks_title_idx",
                   "DROP 0", "branch-local DROP INDEX must work");
    expect_command(connection.get(),
        "ALTER TABLE orders ADD COLUMN branch_label TEXT DEFAULT 'rollout'",
        "ALTER 0", "ALTER TABLE on an inherited table must copy its schema version");
    expect_cell(connection.get(),
        "SELECT branch_label FROM orders WHERE id = 2",
        0, 0, "rollout", "schema-copy ALTER must preserve rows and backfill defaults");
    expect_cell(connection.get(), "SELECT COUNT(*) FROM orders",
                0, 0, "3", "schema copying must preserve every visible row");

    // Result columns carry their PostgreSQL types, so ordinary clients parse
    // booleans, integers, and JSON instead of receiving text.
    expect_command(connection.get(),
        "CREATE TABLE typed (id INTEGER PRIMARY KEY, active BOOLEAN, doc JSONB, label TEXT)",
        "CREATE 0", "typed table must be creatable");
    expect_command(connection.get(),
        "INSERT INTO typed (id, active, doc, label) VALUES (1, TRUE, '{\"a\": 1}', 'x')",
        "INSERT 0 1", "typed table must accept writes");
    {
        auto typed = expect_result(connection.get(),
            "SELECT id, active, doc, label FROM typed", PGRES_TUPLES_OK, "typed SELECT must work");
        const auto id_type = PQftype(typed.get(), 0);
        check(id_type == 20 || id_type == 23, "integer columns must be described as integers");
        check(PQftype(typed.get(), 1) == 16, "boolean columns must be described as bool");
        check(std::string(PQgetvalue(typed.get(), 0, 1)) == "t", "booleans must travel as t/f");
        check(PQftype(typed.get(), 2) == 3802, "jsonb columns must be described as jsonb");
        check(PQftype(typed.get(), 3) == 25, "text columns must be described as text");
    }
    {
        Result prepared(PQprepare(connection.get(), "typed_lookup",
                                  "SELECT active FROM typed WHERE id = $1", 1, nullptr));
        check(PQresultStatus(prepared.get()) == PGRES_COMMAND_OK, PQerrorMessage(connection.get()));
        // PQexecPrepared describes the bound portal, as node-postgres does.
        const char *values[] = {"1"};
        Result executed(PQexecPrepared(connection.get(), "typed_lookup", 1, values, nullptr, nullptr, 0));
        check(PQresultStatus(executed.get()) == PGRES_TUPLES_OK, PQerrorMessage(connection.get()));
        check(PQftype(executed.get(), 0) == 16, "portal Describe must report column types");
        check(std::string(PQgetvalue(executed.get(), 0, 0)) == "t", "prepared booleans must travel as t/f");
        // Describing the statement before binding, as postgres.js does,
        // reports its columns and parameter types without running it.
        Result described(PQdescribePrepared(connection.get(), "typed_lookup"));
        check(PQresultStatus(described.get()) == PGRES_COMMAND_OK, PQerrorMessage(connection.get()));
        check(PQnfields(described.get()) == 1 && std::string(PQfname(described.get(), 0)) == "active" &&
                  PQftype(described.get(), 0) == 16,
              "statement Describe must report result columns and types");
        check(PQnparams(described.get()) == 1, "statement Describe must report the parameter count");
    }

    // Common CREATE TABLE forms: NOT NULL, COLLATE, a table-level PRIMARY KEY
    // (also composite), and casts in INSERT values.
    expect_command(connection.get(),
        "CREATE TABLE notes (\"objectId\" TEXT COLLATE \"C\" NOT NULL, name TEXT COLLATE \"C\", "
        "spec JSONB, hits BIGINT, PRIMARY KEY (\"objectId\"))",
        "CREATE 0", "CREATE TABLE with NOT NULL and a table-level PRIMARY KEY must work");
    expect_command(connection.get(),
        "CREATE TABLE notes__name (key TEXT NOT NULL, token TEXT NOT NULL, PRIMARY KEY (key, token))",
        "CREATE 0", "a composite table-level PRIMARY KEY must work");
    {
        const char *values[] = {"n1", "first", "{\"k\": 1}", "5"};
        Result inserted(PQexecParams(
            connection.get(),
            "INSERT INTO notes (\"objectId\", name, spec, hits) VALUES ($1, $2, $3::jsonb, $4::bigint)",
            4, nullptr, values, nullptr, nullptr, 0));
        check(PQresultStatus(inserted.get()) == PGRES_COMMAND_OK,
              std::string("casts in INSERT values must work: ") + PQresultErrorMessage(inserted.get()));
    }
    expect_cell(connection.get(), "SELECT hits FROM notes WHERE \"objectId\" = 'n1'",
                0, 0, "5", "cast values must be stored");
    expect_command(connection.get(),
        "INSERT INTO notes__name (key, token) VALUES ('n1', 'first'), ('n1', 'note')",
        "INSERT 0 2", "composite-key rows must insert");
    for (const auto *violation : {
             "INSERT INTO notes (\"objectId\", name) VALUES (NULL, 'x')",
             "INSERT INTO notes__name (key, token) VALUES ('n2', NULL)",
             "UPDATE notes__name SET token = NULL WHERE key = 'n1'",
             "INSERT INTO notes__name (key) VALUES ('n3')"}) {
        expect_result(connection.get(), violation, PGRES_FATAL_ERROR,
                      std::string("NOT NULL must be enforced: ") + violation);
    }
    expect_command(connection.get(), "DELETE FROM notes__name WHERE key = 'n1'",
                   "DELETE 2", "deleting rows of a NOT NULL table must work");
    expect_cell(connection.get(), "SELECT COUNT(*) FROM notes__name",
                0, 0, "0", "deleted NOT NULL rows must be gone");
    expect_result(connection.get(), "ALTER TABLE notes ADD COLUMN label TEXT NOT NULL",
                  PGRES_FATAL_ERROR, "ADD COLUMN NOT NULL without a DEFAULT must fail clearly");
    expect_command(connection.get(), "ALTER TABLE notes ADD COLUMN label TEXT NOT NULL DEFAULT 'none'",
                   "ALTER 0", "ADD COLUMN NOT NULL with a DEFAULT must work");
    expect_cell(connection.get(), "SELECT label FROM notes WHERE \"objectId\" = 'n1'",
                0, 0, "none", "the new NOT NULL column must take its default");
    expect_result(connection.get(),
        "CREATE TABLE uniques (id INTEGER PRIMARY KEY, name TEXT, UNIQUE (name))",
        PGRES_FATAL_ERROR, "unsupported table constraints must fail clearly");

    // A simple-query error must not poison an autocommit connection.
    auto syntax_error = execute(connection.get(), "SELECT * FROM missing_table");
    check(PQresultStatus(syntax_error.get()) == PGRES_FATAL_ERROR,
          "invalid SQL must return a PostgreSQL ErrorResponse");
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 1",
                0, 0, "open", "connection must recover after an autocommit error");

    auto updated = execute(connection.get(), "UPDATE orders SET state = 'placed' WHERE id = 1");
    check(PQresultStatus(updated.get()) == PGRES_COMMAND_OK, PQerrorMessage(connection.get()));
    selected = execute(connection.get(), "SELECT state FROM orders WHERE id = 1");
    check(std::string(PQgetvalue(selected.get(), 0, 0)) == "placed",
          "wire-protocol DML must update the workspace branch");
    auto main = setup.checkout("main");
    auto base = main.query("SELECT state FROM orders WHERE id = 1");
    check(std::get<std::string>(base.rows.at(0).at(0)) == "open",
          "wire-protocol DML must not update the source branch");
    bool source_rejected_branch_column = false;
    try {
        (void)main.query("SELECT branch_label FROM orders");
    } catch (const std::exception &) {
        source_rejected_branch_column = true;
    }
    check(source_rejected_branch_column,
          "branch-local ALTER TABLE must not change the source branch schema");
    bool source_rejected_branch_table = false;
    try {
        (void)main.query("SELECT * FROM tasks");
    } catch (const std::exception &) {
        source_rejected_branch_table = true;
    }
    check(source_rejected_branch_table,
          "branch-local CREATE TABLE must not expose the table on the source branch");

    check(PQresultStatus(execute(connection.get(), "BEGIN").get()) == PGRES_COMMAND_OK,
          "BEGIN must succeed");
    check(PQresultStatus(execute(connection.get(),
        "UPDATE orders SET state = 'cancelled' WHERE id = 1").get()) == PGRES_COMMAND_OK,
        "transactional update must succeed");
    check(PQresultStatus(execute(connection.get(), "ROLLBACK").get()) == PGRES_COMMAND_OK,
          "ROLLBACK must succeed");
    selected = execute(connection.get(), "SELECT state FROM orders WHERE id = 1");
    check(std::string(PQgetvalue(selected.get(), 0, 0)) == "placed",
          "ROLLBACK must restore branch state");

    expect_command(connection.get(), "BEGIN", "BEGIN",
                   "a transaction used for failure recovery must begin");
    auto transaction_error = execute(connection.get(), "SELECT * FROM missing_table");
    check(PQresultStatus(transaction_error.get()) == PGRES_FATAL_ERROR &&
              PQtransactionStatus(connection.get()) == PQTRANS_INERROR,
          "an error inside a transaction must enter failed-transaction state");
    auto blocked = execute(connection.get(), "SELECT state FROM orders WHERE id = 1");
    const char *blocked_state = PQresultErrorField(blocked.get(), PG_DIAG_SQLSTATE);
    check(PQresultStatus(blocked.get()) == PGRES_FATAL_ERROR && blocked_state &&
              std::string(blocked_state) == "25P02",
          "queries in a failed transaction must be rejected with SQLSTATE 25P02");
    expect_command(connection.get(), "ROLLBACK", "ROLLBACK",
                   "ROLLBACK must recover a failed transaction");
    check(PQtransactionStatus(connection.get()) == PQTRANS_IDLE,
          "ROLLBACK must restore idle transaction state");
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 1",
                0, 0, "placed", "connection must work after failed-transaction recovery");
    // The same recovery through the extended protocol, as postgres.js sends
    // ROLLBACK, including after a statement the gateway refuses.
    expect_command(connection.get(), "BEGIN", "BEGIN", "an extended-protocol recovery must begin");
    check(PQresultStatus(execute(connection.get(), "CREATE SCHEMA IF NOT EXISTS probe").get()) ==
              PGRES_FATAL_ERROR && PQtransactionStatus(connection.get()) == PQTRANS_INERROR,
          "a refused statement inside a transaction must fail the transaction");
    {
        Result blocked_extended(PQexecParams(connection.get(), "SELECT state FROM orders WHERE id = $1",
                                             1, nullptr, std::array<const char *, 1>{"1"}.data(),
                                             nullptr, nullptr, 0));
        const char *state = PQresultErrorField(blocked_extended.get(), PG_DIAG_SQLSTATE);
        check(state && std::string(state) == "25P02",
              "extended queries in a failed transaction must be rejected with SQLSTATE 25P02");
        Result rolled_back(PQexecParams(connection.get(), "ROLLBACK", 0, nullptr, nullptr, nullptr, nullptr, 0));
        check(PQresultStatus(rolled_back.get()) == PGRES_COMMAND_OK &&
                  PQtransactionStatus(connection.get()) == PQTRANS_IDLE,
              "an extended-protocol ROLLBACK must recover a failed transaction");
    }
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 1",
                0, 0, "placed", "connection must work after extended-protocol recovery");
    expect_command(connection.get(), "BEGIN", "BEGIN",
                   "a committing transaction must begin");
    expect_command(connection.get(),
        "INSERT INTO orders "
        "(id, customer_id, state, amount, note, branch_label) "
        "VALUES (6, 10, 'committed', 30, NULL, 'rollout')",
        "INSERT 0 1", "DML inside a committing transaction must work");
    expect_command(connection.get(), "COMMIT", "COMMIT",
                   "COMMIT must publish a transaction");
    expect_cell(connection.get(), "SELECT state FROM orders WHERE id = 6",
                0, 0, "committed", "committed data must remain visible");
    expect_command(connection.get(), "DELETE FROM orders WHERE id = 6",
                   "DELETE 1", "committed test data must be removable");

    {
        // Terminate closes the connection, so clients waiting for it finish.
        auto closing = connect(port, issued.access_grant->id, issued.database_credential, "orders");
        check(PQstatus(closing.get()) == CONNECTION_OK, PQerrorMessage(closing.get()));
        const int descriptor = dup(PQsocket(closing.get()));
        closing.reset();
        // libpq's socket is non-blocking; wait for the close instead.
        fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) & ~O_NONBLOCK);
        char byte = 0;
        timeval timeout{2, 0};
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const auto received = recv(descriptor, &byte, 1, 0);
        close(descriptor);
        check(received == 0, "the gateway must close a connection after Terminate");
    }

    auto bad = connect(port, issued.access_grant->id, "wrong", "orders");
    check(PQstatus(bad.get()) == CONNECTION_BAD, "wrong grant secret must fail authentication");

    test_principals(runtime, port, data, metadata, issued.access_grant->id, issued.database_credential);
    test_reset_reopens_idle_sessions(runtime, port, data, metadata);

    runtime.close_branch("training", "rollout-wire");
    auto revoked = execute(connection.get(), "SELECT state FROM orders");
    check(PQresultStatus(revoked.get()) == PGRES_FATAL_ERROR,
          "revocation must invalidate an already-open SQL connection");

    connection.reset();
    bad.reset();
    server.stop();
    server_thread.join();
    std::cout << "all PostgreSQL gateway protocol tests passed\n";
    return 0;
}
