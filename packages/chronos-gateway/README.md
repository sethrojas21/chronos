# Chronos gateway

`chronos-gateway` gives an unmodified runtime sandbox access to one branch of a
Chronos workspace through ordinary PostgreSQL and NFSv4 clients. A trusted
coordinator uses the private HTTP control API to create the branch, choose its
database and filesystem access, and attach it to a sandbox. The gateway keeps
the resulting credentials out of Chronos's public workspace model.

The API follows the existing workspace lifecycle. For example,
`POST /v1/workspaces/training/branches` corresponds to
`workspace.create_branch(...)`; SQL and NFS both check out the returned branch.

## Running locally with Docker

`docker/run-local.sh` builds the gateway in a Linux container (the same
environment as CI) and runs a SQL-only gateway backed by PostgreSQL 18, also in
Docker. It works on macOS. The control API is published on `127.0.0.1:7400`
and the PostgreSQL wire port on `localhost:6543`, with workspace `local` and
SQL store `app`. Use `run-local.sh rebuild` after code changes and
`run-local.sh stop` to stop it.

A config without an `nfs` listener runs only the control API and the
PostgreSQL frontend; workspaces in it cannot configure a filesystem. Behind NAT
or container port mapping, set `listeners.postgres_advertise` to the
`host:port` clients should put in database URLs.

## Sandbox and long-lived branches

By default a created branch is a sandbox: a terminal branch owned by its
access grant. Its `ttl_seconds` (default 3600) bounds the access; when the
grant is closed or expires, the reaper deletes the branch after the recovery
window. `PATCH .../branches/{id}` with `ttl_seconds` renews the access.

Pass `"sandbox": false` for a long-lived branch, for example one per agent:

```json
POST /v1/workspaces/training/branches
{"branch_id": "agent-7", "sandbox": false, "databases": ["orders"],
 "metadata": {"agent": "7"}}
```

- It is an ordinary branch that can be branched from and merged.
- `ttl_seconds` is optional and limits only its access. Omitted or `null`
  means the access never expires. Expired access never deletes the branch.
- `metadata` (a JSON object) is stored on the branch. `from_checkpoint`
  creates the branch from a checkpoint instead of `from_branch`.
- Without `databases` or `filesystem`, the branch is created with no access.
- `POST .../branches/{id}/credentials` with `databases`, `filesystem`, and an
  optional `ttl_seconds` issues new database URLs for an existing long-lived
  branch and retires the previous ones. Use it after a gateway restart, since
  access grants are held in memory. Sandboxes receive credentials only when
  created.
- `GET .../branches/{id}` returns the branch with its metadata and current
  access, and never returns credentials. `DELETE` deletes a long-lived branch
  immediately, while for a sandbox it closes access as before.

## Logins and permissions

Besides the per-branch credentials above, a workspace can have principals:
stable SQL logins for agents and people, each allowed a set of branches.

```json
POST /v1/workspaces/training/principals
{"name": "agent-7", "default_branch": "agent/7",
 "branches": ["agent/7", "scratch/*"], "read_only_branches": ["main"]}
```

The answer (201) holds the principal, its `password`, and a `database_urls`
entry per SQL store. The password is shown only here and in the answer to
`rotate`.

| Route | Effect |
| --- | --- |
| `POST .../principals` | create: `name`, `default_branch`, `branches`, `read_only_branches`; 409 if the name exists |
| `GET .../principals` | `{"principals": [...]}`, without secrets |
| `GET .../principals/{name}` | one principal, without secrets |
| `PATCH .../principals/{name}` | replace any of `default_branch`, `branches`, `read_only_branches` |
| `POST .../principals/{name}/rotate` | new password, same name; the old password stops working |
| `DELETE .../principals/{name}` | delete the login |

Names have 1 to 63 characters (letters, digits, `_ - . @`, starting with a
letter or `_`) and are unique per workspace. Unknown fields are refused.
Branches are still created by the branch routes above.

Branch rules:

- A pattern is a branch name, `prefix/*` (every branch below `prefix/`, not
  `prefix` itself), or `*` (every branch).
- A branch is allowed when it matches `branches` or `read_only_branches`. It
  is read-only when it matches `read_only_branches`, even if it also matches
  `branches`.
- `default_branch` must be allowed. Patterns may name branches that do not
  exist yet; connecting to a missing branch fails.

Connecting: use the principal's name as the user and its password, with the
store alias as the database (`workspace/alias` if the same name exists in
several workspaces serving that alias). The connection lands on the default
branch. To start elsewhere, add `-c chronos.branch=NAME` to the startup
options; it can be combined with `-c search_path=...`:

```sh
PGOPTIONS='-c search_path=public -c chronos.branch=main' \
  psql 'postgresql://agent-7:PASSWORD@localhost:6543/orders'
```

- `SET chronos.branch = 'NAME'` switches the connection to another allowed
  branch. It is refused inside a transaction (SQLSTATE `25001`); prepared
  statements and the search path carry over. `RESET chronos.branch` returns to
  the branch chosen at login, and `SHOW chronos.branch` reports the current
  one. `RESET ALL` and `DISCARD ALL` do not change the branch.
- A branch that is not allowed fails with SQLSTATE `42501`, at login or on
  `SET`. On a read-only branch, statements other than queries and session or
  transaction control (`SELECT`, `WITH`, `SHOW`, `SET`, `RESET`, `BEGIN`,
  `COMMIT`, ...) fail with `42501`, as do queries that write (`WITH ...
  DELETE`, `SELECT ... INTO`).
- Every statement re-checks the login: after a rotation or deletion, open
  connections fail, and after a `PATCH` that removes their branch they fail
  with `42501`.

Access grants (the random users of the branch routes) work as before. They are
checked first, a principal name can never have a grant id's form (32
lowercase hex characters), and a grant connection stays on its branch: it may
pass `-c chronos.branch` only with its own branch, and `SET chronos.branch`
fails with `42501`.

Security notes:

- Only the controller token manages principals; there is no self-service.
- Passwords are 32 random bytes (hex), generated by the gateway. Only a
  PBKDF2-HMAC-SHA256 hash (100,000 iterations, 16-byte per-principal salt) is
  stored, in the table `_chronos_gateway_principals` of the workspace's
  metadata database, with the name, default branch, and patterns. SQL clients
  cannot read it: the `_chronos_` namespace is refused on the wire.
- Principals persist across gateway restarts; access grants do not. A gateway
  loads principals at startup and keeps them in memory, so manage them through
  one gateway per metadata database.
- Principals give SQL access only; the filesystem still uses grants.
- Read-only is enforced on the statement, not on functions with side effects
  the store may run inside a query.

## Versioning routes

These routes run Chronos's versioning operations through the chronos-core
Python package, which the gateway embeds: each route calls one function of
`chronos_core.control.operations`, the same function the `chronos` CLI command
calls, so the two behave alike. All need the controller bearer token. Arguments
come from the JSON body, the query string, and the path, and use the CLI's
names. Responses are the API's result as JSON, or `{"error": "..."}` with 404
for an unknown workspace, store, branch, or table, 400 for invalid arguments or
a refused operation, and 500 otherwise. Configured database URLs never appear in
errors.

| Route | Operation (CLI command) | Arguments |
| --- | --- | --- |
| `GET .../branches` | `list_branches` (`branch list`) | `?store` for one store's records with metadata |
| `PUT .../branches/{id}/metadata` | `update_branch_metadata` | `store`, `metadata` |
| `POST .../branches/{id}/reset` | `reset_branch` (`branch reset`) | `checkpoint`; moves the branch back to it in place, keeping its name and metadata. Known limit: SQL connections already open on that branch keep reading its earlier state until they reconnect |
| `POST .../checkpoints` | `create_checkpoint` (201) | `name`, `branch` (default `main`), `metadata` |
| `GET .../checkpoints` | `list_checkpoints` | `?store`, `?branch`, `?metadata_filter` (JSON) |
| `GET .../diff` | `diff` | `?left`, `?right` |
| `POST .../merges/preview` | `merge_preview` | `source`, `target`, `policy` |
| `POST .../merges/apply` | `merge_apply` | `source`, `target`, `policy`, `keep_branch` |
| `POST .../merges/atomic-preview` | `merge_atomic_preview` | `source`, `target`, `policy` |
| `POST .../merges/atomic-apply` | `merge_atomic` | `source`, `target`, `operation_id`, `preview_token`, `policy`, `keep_branch` |
| `POST .../tables` | `register_table` (201) | `store`, `table`, `primary_key`, optional `create_sql` (a CREATE TABLE run first, as `table create`) |
| `GET .../tables/{table}` | `table_info` | `?store` |
| `POST .../tables/{table}/indexes` | `create_index` (201) | `store`, `columns`, `name` |

`.../` is `/v1/workspaces/{workspace}/`. `policy` is `abort_on_conflict` (the
default, as in the CLI), `snapshot_isolation`, or `api-default`. `keep_branch`
(default `false`, as `--keep-branch` in the CLI) continues the source from the
merged target, so a long-lived branch can merge into it again. Each workspace
opens on its first versioning request, with the table structure mode of its
configuration (below), and stays open; its requests run one at a time.

All stores of a gateway workspace share one metadata database, so the workspace
is atomic: use the atomic merge routes, since `merges/apply` is refused as in a
CLI workspace with `shared_metadata_url`. A workspace with several stores
(including a filesystem) refuses diff and merges, because the CLI requires
`merge_tables` for that layout and the gateway configuration has none yet.

## Shared or per-branch table structure

Each workspace chooses how table structure is versioned with
`"schema_branching"` (default `true`):

- `true`: each branch has its own table structure, and SQL clients may run DDL
  on their branch. The Python API and the engine cannot yet diff or merge
  tables in this mode with the gateway's separate metadata database, so the
  versioning routes refuse it.
- `false`: one table structure is shared by every branch, as in the CLI; only
  rows are versioned. Diff, merges, checkpoints, and table info work. A SQL
  client's `CREATE TABLE` (with a primary key, `IF NOT EXISTS` honored) creates
  and registers the table for every branch, and `CREATE INDEX` creates a
  Chronos index; a partial index (`WHERE`) becomes a plain index on the same
  columns. These run outside the client's transaction: the table or index
  exists on every branch as soon as the statement returns, and `ROLLBACK` does
  not remove it. Other DDL (`ALTER`, `DROP`, unique or expression indexes,
  tables without a primary key, PostgreSQL schemas) is refused with SQLSTATE
  `0A000`. The translation is `chronos_core.control.ddl`.

`information_schema.columns` reflects a shared table's columns.
`pg_indexes` does not yet list Chronos indexes.

### PostgreSQL schemas and search_path

With PostgreSQL stores, a shared-structure workspace supports schemas as
namespaces that every branch shares (schemas themselves are not versioned):

- `CREATE SCHEMA [IF NOT EXISTS] name` creates the schema in the store.
- A table in a schema is registered as `schema.table` and versioned like any
  other: `CREATE TABLE "s"."t" (...)`, or an unqualified `CREATE TABLE`, which
  goes to the first schema of the connection's search path.
- Each connection keeps its own search path, set by `SET search_path TO ...`
  or by the startup option `-c search_path=...` (as libpq's and
  node-postgres's `options` send it), cleared by `RESET search_path`, and
  reported by `SHOW search_path`. Unqualified table names in queries and in
  INSERT, UPDATE, and DELETE resolve along it to the first registered table;
  qualified names are used as written. `current_schema()` returns the first
  schema of the path.
- Index names are per schema, as in PostgreSQL: an index on a schema's table
  is registered as `schema.index`.

Known limits: `DROP SCHEMA` is refused; `set_config('search_path', ...)` is
not tracked; `current_schemas()` and catalog views other than
`information_schema.columns` and `information_schema.schemata`/`tables` are
answered by the store as is; and SQLite stores have no schemas.

`run-local.sh` runs workspace `local` with the default and workspace `shared`
with `"schema_branching": false`, each on its own databases.

`run-local.sh` installs chronos-core into its build volume. Elsewhere, install
it for the Python the gateway links (`python3 -m pip install
packages/chronos-core`, or set `PYTHONPATH`). Without it these routes answer
501 and the rest of the gateway works as before.

The executable contains the control server, PostgreSQL wire frontend,
ChronosFS capability view, and NFS-Ganesha server in one process. Ganesha is
loaded as a library and reads the in-process ChronosFS FUSE mount through its
VFS adapter; no separate Chronos filesystem RPC daemon is used.

Build and test from the repository root:

```sh
cmake -S packages/chronos-gateway -B build/gateway -DCMAKE_BUILD_TYPE=Release
cmake --build build/gateway -j
ctest --test-dir build/gateway --output-on-failure
```

`chronos_gateway_postgres_tests` is a pgwire compatibility matrix. It connects
with libpq and covers joins, CTEs, aggregates, subqueries, set operations,
prepared parameters, transactions and error recovery, DML, and branch-local
DDL. The default run uses temporary SQLite data and metadata stores for a fast
test. Set both `CHRONOS_GATEWAY_TEST_POSTGRES_DATA_URL` and
`CHRONOS_GATEWAY_TEST_POSTGRES_METADATA_URL` to empty PostgreSQL databases to
run the same matrix against PostgreSQL. CI runs both modes.

Branch-local `CREATE TABLE` accepts a primary key either inline or as a
table-level `PRIMARY KEY (...)`, including composite keys, plus `NOT NULL`,
`NULL`, constant `DEFAULT`, and `COLLATE`. Every table needs a primary key, and
other table constraints are refused. Chronos enforces `NOT NULL` on every live
row it writes. `ALTER TABLE ... ADD COLUMN ... NOT NULL` needs a `DEFAULT`.
`INSERT ... VALUES` accepts casts such as `$1::jsonb`, and
`INSERT ... ON CONFLICT (primary key) DO UPDATE SET ...` updates existing rows
with PostgreSQL's meaning of `EXCLUDED`.

Result columns carry PostgreSQL type OIDs, so clients parse booleans, integers,
floats, numeric, JSON/JSONB, timestamps, and bytea natively, and booleans
travel as `t`/`f`. With PostgreSQL stores the types are the server's own. With
SQLite stores they follow each column's declared type, and computed
expressions are typed from their values. Describing a bound portal reports
these types (node-postgres and `PQexecPrepared` use this path). Describing a
prepared statement before binding (postgres.js uses this path) reports its
columns and parameter types: the gateway prepares the rewritten query on the
store without running it. On SQLite, parameters and computed columns are
described as unknown or text.

A refused or failing statement inside a transaction leaves the transaction
failed until `ROLLBACK` (or `COMMIT`, which rolls back), through either the
simple or the extended protocol, as in PostgreSQL.

The test suite includes an opt-in end-to-end test against E2B Embed running on
the same machine. It creates real Firecracker microVMs and exercises both the
PostgreSQL and NFS paths; it never uses E2B Cloud. See the tutorial for the
one-time host setup and the required test command.

The complete setup and E2B walkthrough are in the
[verl and E2B tutorial](../../docs/tutorials/rl-data-sandbox.md). The example runtime
configuration is in
[`deploy/chronos-gateway`](../../deploy/chronos-gateway/chronos-gateway.example.json).
The [`chronos-e2b`](../chronos-e2b) coordinator adapter automates branch
creation, database URL injection, NFS attachment, synchronization, and cleanup.
