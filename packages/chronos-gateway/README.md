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
