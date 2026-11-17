"""CREATE TABLE and CREATE INDEX for stores whose table structure is shared.

When every branch shares one table structure, a table or index is not a
branch change: it is created once and registered for every branch. This
module turns a client's CREATE TABLE or CREATE INDEX into the same Python API
calls `chronos table create`, `table register`, and `table index` make, so
SQL clients (such as those of the gateway's PostgreSQL frontend) can keep
issuing ordinary DDL.

These calls are not part of the client's transaction: the table or index
exists on every branch as soon as the statement returns, and a rollback does
not remove it.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from chronos_core.control import operations

if TYPE_CHECKING:
    from chronos_core.workspace import ChronosWorkspaceContext


class SharedDDLError(ValueError):
    """A statement shared-structure stores cannot run, with its PostgreSQL SQLSTATE."""

    def __init__(self, message: str, sqlstate: str = "0A000"):
        super().__init__(message)
        self.sqlstate = sqlstate


@dataclass(frozen=True)
class CreateTable:
    table: str
    primary_key: tuple[str, ...]
    if_not_exists: bool
    sql: str


@dataclass(frozen=True)
class CreateIndex:
    table: str
    columns: tuple[str, ...]
    name: str | None
    if_not_exists: bool


def parse_shared_ddl(sql: str) -> CreateTable | CreateIndex:
    """Read one CREATE TABLE or CREATE INDEX; refuse anything else."""
    import sqlglot
    from sqlglot import exp

    try:
        statements = [statement for statement in sqlglot.parse(sql, read="postgres") if statement is not None]
    except sqlglot.errors.ParseError as error:
        raise SharedDDLError(f"could not parse statement: {error}", "42601") from error
    if len(statements) != 1 or not isinstance(statements[0], exp.Create):
        raise SharedDDLError(_SHARED_ONLY)
    statement = statements[0]
    kind = str(statement.args.get("kind") or "").upper()
    if_not_exists = bool(statement.args.get("exists"))
    if kind == "TABLE":
        schema = statement.this
        if not isinstance(schema, exp.Schema) or not isinstance(schema.this, exp.Table):
            raise SharedDDLError("CREATE TABLE ... AS and other forms without columns are not supported")
        table = _table_name(schema.this)
        primary_key: list[str] = []
        for item in schema.expressions:
            if isinstance(item, exp.ColumnDef) and any(
                isinstance(constraint.kind, exp.PrimaryKeyColumnConstraint) for constraint in item.constraints
            ):
                primary_key.append(item.name)
            elif isinstance(item, exp.PrimaryKey):
                primary_key.extend(column.name for column in item.expressions)
        if not primary_key:
            raise SharedDDLError(f"table {table!r} needs a primary key to be versioned by Chronos")
        return CreateTable(table, tuple(primary_key), if_not_exists, sql)
    if kind == "INDEX":
        if statement.args.get("unique"):
            raise SharedDDLError("unique indexes are not supported; Chronos indexes only speed up reads")
        index = statement.this
        table = _table_name(index.args["table"])
        params = index.args.get("params")
        columns: list[str] = []
        for item in (params.args.get("columns") if params else None) or []:
            column = item.this if isinstance(item, exp.Ordered) else item
            if not isinstance(column, exp.Column):
                raise SharedDDLError("indexes on expressions are not supported; index plain columns")
            columns.append(column.name)
        if not columns:
            raise SharedDDLError("CREATE INDEX needs at least one column")
        # A partial index (WHERE ...) becomes a plain index on the same
        # columns: it covers the same rows, so results are unchanged.
        name = index.name or None
        return CreateIndex(table, tuple(columns), name, if_not_exists)
    raise SharedDDLError(_SHARED_ONLY)


def apply_shared_ddl(workspace: ChronosWorkspaceContext, store: str, sql: str) -> dict[str, Any]:
    """Create and register a table, or create an index, for every branch.

    Returns {"tag": command tag, "created": bool}; IF NOT EXISTS on an existing
    table or index returns created False.
    """
    statement = parse_shared_ddl(sql)
    info = operations.table_info(workspace, store, statement.table)
    if isinstance(statement, CreateTable):
        if info.registered:
            if statement.if_not_exists:
                return {"tag": "CREATE TABLE", "created": False}
            raise SharedDDLError(f'relation "{statement.table}" already exists', "42P07")
        if not info.exists:
            operations.create_table(workspace, store, statement.sql)
        operations.register_table(workspace, store, statement.table, list(statement.primary_key))
        return {"tag": "CREATE TABLE", "created": True}
    if not info.registered:
        raise SharedDDLError(f'relation "{statement.table}" does not exist', "42P01")
    for index in info.indexes:
        same_name = statement.name is not None and index.name == statement.name
        same_columns = statement.name is None and tuple(index.columns) == statement.columns
        if same_name or same_columns:
            if statement.if_not_exists or same_columns:
                return {"tag": "CREATE INDEX", "created": False}
            raise SharedDDLError(f'relation "{statement.name}" already exists', "42P07")
    operations.create_index(workspace, store, statement.table, list(statement.columns), statement.name)
    return {"tag": "CREATE INDEX", "created": True}


_SHARED_ONLY = (
    "table structure is shared by every branch in this workspace; only CREATE TABLE "
    "(with a primary key) and CREATE INDEX are accepted, and they apply to every branch"
)


def _table_name(table: Any) -> str:
    schema = table.args.get("db")
    if schema is not None and schema.name not in ("", "public"):
        raise SharedDDLError("PostgreSQL schemas are not supported; use table-name prefixes")
    return table.name
