"""Versioning control for the Chronos gateway, run in its embedded interpreter.

The gateway's HTTP routes map a request to an operation name and arguments and
call GatewayControl.handle. Every operation runs a function from
chronos_core.control.operations against a workspace opened as the CLI opens it,
with branch-local schema as the gateway's native stores use, so the gateway
and the CLI behave the same.
"""
from __future__ import annotations

import json
import sys
import threading
from collections.abc import Callable
from contextlib import ExitStack
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any
from urllib.parse import urlsplit

from chronos_core.control import operations
from chronos_core.control.encoding import json_default
from chronos_core.control.redaction import redact_message

if TYPE_CHECKING:
    from chronos_core.cli._config import WorkspaceConfig
    from chronos_core.workspace import ChronosWorkspaceContext


class ControlError(Exception):
    """A request error with its HTTP status."""

    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


# Merge policies as the CLI accepts them; the CLI's default is abort_on_conflict.
_POLICIES = {"api-default": None, "abort_on_conflict": "abort_on_conflict",
             "snapshot_isolation": "snapshot_isolation"}


def _text(args: dict[str, Any], name: str, *, required: bool = True, default: str | None = None) -> Any:
    value = args.get(name)
    if value is None:
        if required:
            raise ControlError(400, f"{name} is required")
        return default
    if not isinstance(value, str) or not value:
        raise ControlError(400, f"{name} must be a non-empty string")
    return value


def _object(args: dict[str, Any], name: str, *, required: bool = False) -> dict[str, Any] | None:
    value = args.get(name)
    if isinstance(value, str):
        # Query parameters carry JSON objects as text.
        try:
            value = json.loads(value)
        except ValueError as error:
            raise ControlError(400, f"{name} must be a JSON object") from error
    if value is None:
        if required:
            raise ControlError(400, f"{name} is required")
        return None
    if not isinstance(value, dict):
        raise ControlError(400, f"{name} must be a JSON object")
    return value


def _names(args: dict[str, Any], name: str) -> list[str]:
    value = args.get(name)
    if isinstance(value, str):
        value = [value]
    if not isinstance(value, list) or not value or any(not isinstance(item, str) or not item for item in value):
        raise ControlError(400, f"{name} must be a non-empty array of names")
    return value


def _flag(args: dict[str, Any], name: str) -> bool:
    value = args.get(name)
    if value is None:
        return False
    if isinstance(value, str):
        # Query parameters carry booleans as text.
        value = {"true": True, "false": False}.get(value, value)
    if not isinstance(value, bool):
        raise ControlError(400, f"{name} must be true or false")
    return value


def _policy(args: dict[str, Any]) -> str | None:
    value = _text(args, "policy", required=False, default="abort_on_conflict")
    if value not in _POLICIES:
        raise ControlError(400, f"policy must be one of {', '.join(_POLICIES)}")
    return _POLICIES[value]


@dataclass
class _Workspace:
    config: WorkspaceConfig
    urls: tuple[str, ...]
    # Diff and merge need every SQL store scoped to its own tables when several
    # participants share one metadata plane, as the CLI requires.
    merge_scoped: bool
    # Branch-local schema, as the gateway's native stores are opened.
    schema_branching: bool = True
    lock: threading.Lock = field(default_factory=threading.Lock)
    resources: ExitStack | None = None
    context: ChronosWorkspaceContext | None = None


def _kind(url: str) -> str:
    scheme = urlsplit(url).scheme
    if scheme in ("postgres", "postgresql"):
        return "postgresql"
    if scheme in ("sqlite", "duckdb"):
        return scheme
    raise ValueError("unsupported database URL scheme")


def _workspace_config(name: str, entry: dict[str, Any]) -> _Workspace:
    from chronos_core.cli._config import DatabaseConfig, StoreConfig, WorkspaceConfig

    stores: dict[str, StoreConfig] = {}
    urls: list[str] = []
    metadata_urls: set[str] = set()
    for alias, store in (entry.get("stores") or {}).items():
        data_url, metadata_url = store["data_url"], store.get("metadata_url") or None
        merge_tables = tuple(store.get("merge_tables") or ()) or None
        stores[alias] = StoreConfig(_kind(data_url), DatabaseConfig(data_url, metadata_url), merge_tables)
        urls.extend(url for url in (data_url, metadata_url) if url)
        metadata_urls.add(metadata_url or data_url)
    filesystem = None
    if entry.get("filesystem"):
        data_url = entry["filesystem"]["data_url"]
        metadata_url = entry["filesystem"].get("metadata_url") or None
        filesystem = DatabaseConfig(data_url, metadata_url)
        urls.extend(url for url in (data_url, metadata_url) if url)
        metadata_urls.add(metadata_url or data_url)
    if not stores and filesystem is None:
        raise ValueError(f"workspace {name!r} has no stores")
    # The gateway requires one metadata plane per workspace; share it so
    # atomic merges work as in a CLI workspace with shared_metadata_url.
    shared = next(iter(metadata_urls)) if len(metadata_urls) == 1 else None
    participants = len(stores) + (filesystem is not None)
    merge_scoped = participants == 1 or all(store.merge_tables for store in stores.values())
    config = WorkspaceConfig(Path("."), stores, shared, filesystem)
    return _Workspace(config, tuple(urls), merge_scoped, bool(entry.get("schema_branching", True)))


# Each handler validates its arguments against the workspace configuration
# before anything opens, and returns the call to run on the open workspace.
Call = Callable[["ChronosWorkspaceContext"], Any]


def _store(config: WorkspaceConfig, args: dict[str, Any], *, required: bool = True) -> str | None:
    store = _text(args, "store", required=required)
    if store is not None and store not in config.stores:
        raise ControlError(404, f"unknown store {store!r}")
    return store


def _list_branches(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store = _store(c, a, required=False)
    return lambda ws: operations.list_branches(ws, store)


def _update_branch_metadata(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store, branch, metadata = _store(c, a), _text(a, "branch"), _object(a, "metadata", required=True)
    return lambda ws: operations.update_branch_metadata(ws, store, branch, metadata)


def _create_checkpoint(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    name, branch = _text(a, "name"), _text(a, "branch", required=False, default="main")
    metadata = _object(a, "metadata")
    return lambda ws: operations.create_checkpoint(ws, name, branch, metadata)


def _list_checkpoints(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store, branch = _store(c, a), _text(a, "branch", required=False)
    metadata_filter = _object(a, "metadata_filter")
    return lambda ws: operations.list_checkpoints(ws, store, branch, metadata_filter)


def _diff(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    left, right = _text(a, "left"), _text(a, "right")
    return lambda ws: operations.diff(ws, left, right)


def _merge(name: str) -> Callable[[WorkspaceConfig, dict[str, Any]], Call]:
    def parse(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
        source, target, policy = _text(a, "source"), _text(a, "target"), _policy(a)
        return lambda ws: getattr(operations, name)(ws, source, target, policy)
    return parse


def _merge_apply(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    source, target, policy = _text(a, "source"), _text(a, "target"), _policy(a)
    if _flag(a, "keep_branch"):
        return lambda ws: operations.merge_apply(ws, source, target, policy, keep_branch=True)
    return lambda ws: operations.merge_apply(ws, source, target, policy)


def _merge_atomic(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    source, target, policy = _text(a, "source"), _text(a, "target"), _policy(a)
    operation_id, preview_token = _text(a, "operation_id"), _text(a, "preview_token", required=False)
    options = {"keep_branch": True} if _flag(a, "keep_branch") else {}
    return lambda ws: operations.merge_atomic(
        ws, source, target, operation_id=operation_id, preview_token=preview_token, policy=policy, **options)


def _register_table(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store, table, primary_key = _store(c, a), _text(a, "table"), _names(a, "primary_key")
    create_sql = _text(a, "create_sql", required=False)

    def run(ws: Any) -> Any:
        # Optional CREATE TABLE first, as `chronos table create` then `table register`.
        if create_sql is not None:
            operations.create_table(ws, store, create_sql)
        return operations.register_table(ws, store, table, primary_key)
    return run


def _table_info(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store, table = _store(c, a), _text(a, "table")
    return lambda ws: operations.table_info(ws, store, table)


def _apply_ddl(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    from chronos_core.control.ddl import apply_shared_ddl

    store, sql = _store(c, a), _text(a, "sql")
    search_path = a.get("search_path") or None
    if search_path is not None and (not isinstance(search_path, list)
                                    or not all(isinstance(item, str) for item in search_path)):
        raise ControlError(400, "search_path must be a list of schema names")
    return lambda ws: apply_shared_ddl(ws, store, sql, search_path)


def _create_index(c: WorkspaceConfig, a: dict[str, Any]) -> Call:
    store, table, columns = _store(c, a), _text(a, "table"), _names(a, "columns")
    name = _text(a, "name", required=False)
    return lambda ws: operations.create_index(ws, store, table, columns, name)


# operation -> (handler, arguments it accepts, success status, needs merge scope)
_OPERATIONS: dict[str, tuple[Callable[[Any, dict[str, Any]], Call], frozenset[str], int, bool]] = {
    "list_branches": (_list_branches, frozenset({"store"}), 200, False),
    "update_branch_metadata": (_update_branch_metadata, frozenset({"store", "branch", "metadata"}), 200, False),
    "create_checkpoint": (_create_checkpoint, frozenset({"name", "branch", "metadata"}), 201, False),
    "list_checkpoints": (_list_checkpoints, frozenset({"store", "branch", "metadata_filter"}), 200, False),
    "diff": (_diff, frozenset({"left", "right"}), 200, True),
    "merge_preview": (_merge("merge_preview"), frozenset({"source", "target", "policy"}), 200, True),
    "merge_apply": (_merge_apply, frozenset({"source", "target", "policy", "keep_branch"}), 200, True),
    "merge_atomic_preview": (_merge("merge_atomic_preview"), frozenset({"source", "target", "policy"}), 200, True),
    "merge_atomic": (
        _merge_atomic, frozenset({"source", "target", "policy", "operation_id", "preview_token", "keep_branch"}),
        200, True),
    "register_table": (_register_table, frozenset({"store", "table", "primary_key", "create_sql"}), 201, False),
    "table_info": (_table_info, frozenset({"store", "table"}), 200, False),
    "create_index": (_create_index, frozenset({"store", "table", "columns", "name"}), 201, False),
    # CREATE TABLE / CREATE INDEX from SQL clients of a shared-structure workspace.
    "apply_ddl": (_apply_ddl, frozenset({"store", "sql", "search_path"}), 200, False),
}


def _status(error: BaseException) -> int:
    from chronos_core.branching import BranchingError, BranchNotFoundError, TableNotRegisteredError
    from chronos_core.cli._config import WorkspaceConfigError
    from chronos_core.workspace import ChronosFSError, FilesystemStoreError

    if isinstance(error, ControlError):
        return error.status
    if isinstance(error, (BranchNotFoundError, TableNotRegisteredError)):
        return 404
    if isinstance(error, RuntimeError):
        # The native engine reports its errors, missing branches included, as
        # RuntimeError, which the CLI also treats as an operation's failure.
        return 404 if " not found" in str(error) else 400
    if isinstance(error, (BranchingError, ChronosFSError, FilesystemStoreError, WorkspaceConfigError,
                          ValueError, TypeError)):
        return 400
    return 500


class GatewayControl:
    """Serve versioning operations for the gateway's configured workspaces.

    config: {"workspaces": {name: {"stores": {alias: {"data_url", "metadata_url",
    "merge_tables"?}}, "filesystem": {"data_url", "metadata_url"} | null}}}.
    Each workspace opens on its first request and stays open; requests to one
    workspace run one at a time.
    """

    def __init__(self, config: str | dict[str, Any]):
        data = json.loads(config) if isinstance(config, str) else config
        self._workspaces = {
            name: _workspace_config(name, entry) for name, entry in (data.get("workspaces") or {}).items()
        }
        self._all_urls = tuple(url for entry in self._workspaces.values() for url in entry.urls)

    def handle(self, request_json: str) -> str:
        """Run {"operation", "workspace", "args"} and return {"status", "body"} as JSON."""
        urls = self._all_urls
        try:
            request = json.loads(request_json)
            if not isinstance(request, dict):
                raise ControlError(400, "request must be a JSON object")
            name = request.get("workspace")
            entry = self._workspaces.get(name) if isinstance(name, str) else None
            if entry is None:
                raise ControlError(404, "unknown workspace")
            urls = entry.urls
            found = _OPERATIONS.get(request.get("operation"))
            if found is None:
                raise ControlError(400, "unknown operation")
            handler, accepted, status, needs_scope = found
            args = request.get("args") or {}
            if not isinstance(args, dict):
                raise ControlError(400, "args must be a JSON object")
            unexpected = sorted(set(args) - accepted)
            if unexpected:
                raise ControlError(400, f"unexpected arguments: {', '.join(unexpected)}")
            if request.get("operation") == "apply_ddl" and entry.schema_branching:
                raise ControlError(400, "apply_ddl is only for workspaces with shared table structure")
            run = handler(entry.config, args)
            if needs_scope and not entry.merge_scoped:
                raise ControlError(
                    400, "diff and merge need merge_tables on every SQL store when a workspace has "
                    "several stores sharing one metadata plane")
            with entry.lock:
                result = run(self._open(entry))
            body = json.dumps({"status": status, "body": result}, default=json_default,
                              ensure_ascii=False, allow_nan=False)
            return body
        except Exception as error:
            status = _status(error)
            message = redact_message(str(error), urls=urls) or type(error).__name__
            if status >= 500:
                print(f"chronos-gateway control: {type(error).__name__}: {message}", file=sys.stderr, flush=True)
            body: dict[str, Any] = {"error": message}
            sqlstate = getattr(error, "sqlstate", None)
            if isinstance(sqlstate, str):
                body["sqlstate"] = sqlstate  # for SQL clients of the gateway
            return json.dumps({"status": status, "body": body})

    @staticmethod
    def _open(entry: _Workspace) -> ChronosWorkspaceContext:
        if entry.context is None:
            from chronos_core.cli._config import open_workspace

            resources = ExitStack()
            try:
                context = resources.enter_context(open_workspace(entry.config, schema_branching=entry.schema_branching))
            except BaseException:
                resources.close()
                raise
            entry.resources, entry.context = resources, context
        return entry.context

    def close(self) -> None:
        """Close every open workspace."""
        for entry in self._workspaces.values():
            with entry.lock:
                resources, entry.resources, entry.context = entry.resources, None, None
                if resources is not None:
                    resources.close()
