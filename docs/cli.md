# Chronos CLI

The `chronos` command is a one-off wrapper around the public relational Python
API. Each invocation opens the selected database, calls the API, prints its
return value, and closes the connection. It does not remember a current branch
or keep a background connection open.

## Install and choose a database

The CLI ships with `chronos-core`. Follow [installation](installation.md) for
native build dependencies. From the repository root, install the workspace package:

```sh
uv sync --package chronos-core
uv run --package chronos-core chronos --help
```

You do not need to activate the virtual environment when using `uv run`.
An installed wheel also provides `chronos`; `python -m chronos_core.cli` invokes
the same application.

On macOS with Homebrew Python, build against the same SQLite library Python uses.
If Python uses Homebrew SQLite, explicitly select it when rebuilding:

```sh
brew install sqlite libpq boost
CMAKE_PREFIX_PATH="$(brew --prefix libpq)" \
uv sync --package chronos-core --reinstall-package chronos-core \
  -C "cmake.define.SQLite3_INCLUDE_DIR=$(brew --prefix sqlite)/include" \
  -C "cmake.define.SQLite3_LIBRARY=$(brew --prefix sqlite)/lib/libsqlite3.dylib"
```

Mixing Python's Homebrew SQLite connection with a native extension linked to
macOS's SQLite can crash the process. After a custom build, use
`uv run --no-sync --package chronos-core chronos ...` to run the installed build
without syncing it again.

Set the database URL in your terminal. This example uses a persistent local file:

```sh
mkdir -p .chronos
export CHRONOS_DATABASE_URL="sqlite:///$PWD/.chronos/demo.sqlite"
```

This keeps the demo in `.chronos/` under your current directory. The absolute URL
continues to select that file if you change directories. Files ending in `.sqlite`
and their SQLite journal files are ignored by this repository's Git configuration.
SQLite creates a missing database file; its parent directory must already exist.
PostgreSQL URLs can also be supplied, for example through an environment variable
populated by your secret-management tooling. No PostgreSQL server is started by
the CLI. A `.env` file is not loaded automatically.

Override the environment variable by placing the shared flag before the command:

```sh
uv run --package chronos-core chronos \
  --database-url "sqlite:///$PWD/.chronos/other.sqlite" branch list
```

Help does not require a database URL and does not open a connection.

## Create and register a table

For a fresh database, create an ordinary table directly through the CLI, then
register it with Chronos:

```sh
uv run --package chronos-core chronos table create \
  'CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)'
uv run --package chronos-core chronos table register items --primary-key id
```

`table create` wraps the public database adapter's `execute()` and `commit()`
methods for initial setup. It accepts one CREATE TABLE statement directly, through
`--file`, or through stdin. This creates an ordinary database table; it is not a
branch-local schema change. Run it once before registration and managed access.
The branch-session `execute` command continues to use the API's default behavior.

If your database already contains the application table, skip `table create` and
register it directly. For composite keys, repeat `--primary-key`. Register each
table once. Registration copies existing data into Chronos-managed storage;
subsequent managed reads and writes must use branch sessions. Direct access to
the original table bypasses Chronos. See [integration](integration.md) before
adopting application data.

## Branch, query, checkpoint, and merge

Run this walkthrough against the newly prepared database:

```sh
uv run --package chronos-core chronos branch list
uv run --package chronos-core chronos branch create experiment --from main

uv run --package chronos-core chronos execute \
  'INSERT INTO items (id, value) VALUES (:id, :value)' \
  --branch experiment --params '{"id": 1, "value": "hello"}'

uv run --package chronos-core chronos query \
  'SELECT * FROM items WHERE id = :id' \
  --branch experiment --params '{"id": 1}'

uv run --package chronos-core chronos checkpoint create saved --branch experiment
uv run --package chronos-core chronos checkpoint list --branch experiment
uv run --package chronos-core chronos diff main experiment
uv run --package chronos-core chronos diff main experiment --table items
uv run --package chronos-core chronos merge preview experiment main
uv run --package chronos-core chronos merge apply experiment main
uv run --package chronos-core chronos query 'SELECT * FROM items'
uv run --package chronos-core chronos branch delete experiment
```

Queries, writes, and checkpoint creation default to branch `main`. Branch creation
defaults to `--from main`. Checkpoint listing without `--branch` lists all
checkpoints. Both merge commands accept `--policy abort_on_conflict` (the default)
or `--policy snapshot_isolation`. These are API policies; snapshot isolation does
not mean force-overwriting conflicts. API validation and conflict behavior apply
unchanged.

## SQL input

Both `query` and `execute` accept one SQL source:

```sh
# Direct text, including multiline text inside shell quotes.
uv run --package chronos-core chronos query 'SELECT * FROM items'

# A UTF-8 file.
uv run --package chronos-core chronos query --file query.sql

# Piped input, automatically detected when no other source is supplied.
printf 'SELECT * FROM items\n' | uv run --package chronos-core chronos query

# Explicit stdin; type SQL and finish with Ctrl-D in an interactive terminal.
uv run --package chronos-core chronos query --stdin
```

Empty SQL and conflicting explicit sources are rejected. With no SQL source in an
interactive terminal, the CLI shows an input error rather than opening an editor.
`--params` must be a JSON object of named parameter values. SQL and parameters are
passed directly to `session.query()` or `session.execute()`; files are not split
into a script of multiple statements. Supported SQL is determined by the API.

## Results and errors

Non-`None` API return values are printed as JSON to stdout:

- Queries produce an array of row objects; no rows produces `[]`.
- Writes produce the API's result fields, such as `{"rowcount": 1}`.
- Lists, checkpoints, diffs, and merges preserve the API result field names.
- Methods returning `None`, including successful branch creation/deletion and
  table registration, produce no output.

Dates and times use ISO strings, decimals use strings to preserve precision, and
bytes use `{"encoding": "base64", "data": "..."}` objects. Unsupported values
produce an error instead of silently changing their representation. No query
results are cached or saved unless you redirect stdout:

```sh
uv run --package chronos-core chronos query 'SELECT * FROM items' > rows.json
uv run --package chronos-core chronos query 'SELECT * FROM items' | python -m json.tool
```

Exit code 0 means the API call and output completed successfully. Expected
operational failures are reported on stderr with exit code 1; invalid command-line
input uses exit code 2. A merge preview may successfully return conflicts as data;
merge application follows the API's rejection behavior. Errors do not emit a JSON
success result.

This first version uses the default interval backend with one database per
invocation. Workspace coordination, a persistent interactive shell, advanced
connection tuning, and custom Python merge callbacks are not exposed.
