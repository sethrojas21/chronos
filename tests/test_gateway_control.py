"""The gateway's Python control entry point, against SQLite stores without the gateway."""
from __future__ import annotations

import json
from functools import partial
import sqlite3
from pathlib import Path
from typing import Any

import pytest

from chronos_core.control import gateway as gateway_module
from chronos_core.control.gateway import GatewayControl


def _control(tmp_path: Path, **store: Any) -> GatewayControl:
    database = tmp_path / "app.sqlite"
    with sqlite3.connect(database) as connection:
        connection.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, state TEXT)")
    entry = {"data_url": f"sqlite:///{database}", **store}
    return GatewayControl(json.dumps({"workspaces": {"local": {"stores": {"app": entry}}}}))


def _call(control: GatewayControl, operation: str, workspace: str = "local", **args: Any) -> tuple[int, Any]:
    response = json.loads(control.handle(json.dumps(
        {"operation": operation, "workspace": workspace, "args": args})))
    return response["status"], response["body"]


@pytest.fixture
def control(tmp_path: Path):
    control = _control(tmp_path)
    yield control
    control.close()


def _with_feature_branch(control: GatewayControl) -> None:
    assert _call(control, "register_table", store="app", table="orders", primary_key=["id"])[0] == 201
    workspace = control._open(control._workspaces["local"])
    workspace.stores["app"].checkout("main").execute("INSERT INTO orders (id, state) VALUES (1, 'new')")
    workspace.create_branch("feature")
    workspace.stores["app"].checkout("feature").execute("UPDATE orders SET state = 'paid' WHERE id = 1")


def test_branches_checkpoints_and_diff(control: GatewayControl) -> None:
    _with_feature_branch(control)
    assert _call(control, "list_branches") == (200, ["feature", "main"])
    status, records = _call(control, "list_branches", store="app")
    assert status == 200 and {record["branch_id"] for record in records} == {"feature", "main"}

    status, record = _call(control, "update_branch_metadata", store="app", branch="feature",
                           metadata={"agent": "a1"})
    assert status == 200 and record["metadata"] == {"agent": "a1"}

    status, created = _call(control, "create_checkpoint", name="cp1", branch="feature", metadata={"run": 1})
    assert status == 201 and created["app"]["checkpoint_id"] == "cp1"
    # Query parameters carry the metadata filter as JSON text.
    status, listed = _call(control, "list_checkpoints", store="app", metadata_filter='{"run": 1}')
    assert status == 200 and [item["checkpoint_id"] for item in listed] == ["cp1"]
    assert _call(control, "list_checkpoints", store="app", metadata_filter={"run": 2}) == (200, [])

    status, diff = _call(control, "diff", left="main", right="feature")
    assert status == 200
    assert diff["app"]["changes"][0]["after"] == {"id": 1, "state": "paid"}


def test_tables_and_indexes(control: GatewayControl) -> None:
    assert _call(control, "register_table", store="app", table="orders", primary_key="id") == (201, None)
    status, index = _call(control, "create_index", store="app", table="orders", columns=["state"])
    assert status == 201 and index["columns"] == ["state"]
    status, body = _call(control, "create_index", store="app", table="missing", columns=["state"])
    assert status == 404
    # describe_table refuses under branch-local schema, as in the CLI with schema branching.
    status, body = _call(control, "table_info", store="app", table="orders")
    assert status == 400 and "branch-local schemas" in body["error"]


def test_request_errors(control: GatewayControl) -> None:
    assert _call(control, "list_branches", workspace="other")[0] == 404
    assert _call(control, "list_branches", store="missing") == (404, {"error": "unknown store 'missing'"})
    assert _call(control, "drop_everything") == (400, {"error": "unknown operation"})
    assert _call(control, "diff", left="main") == (400, {"error": "right is required"})
    assert _call(control, "diff", left="main", right="feature", extra="1") == (
        400, {"error": "unexpected arguments: extra"})
    assert _call(control, "update_branch_metadata", store="app", branch="main", metadata="[1]")[0] == 400
    assert _call(control, "merge_preview", source="a", target="main", policy="any")[0] == 400
    _call(control, "register_table", store="app", table="orders", primary_key=["id"])
    assert _call(control, "diff", left="main", right="ghost")[0] == 404
    assert _call(control, "update_branch_metadata", store="app", branch="ghost", metadata={})[0] == 404
    assert json.loads(control.handle("[]"))["status"] == 400


def test_merges_call_operations_with_cli_defaults(control: GatewayControl, monkeypatch) -> None:
    calls: list[tuple[str, tuple[Any, ...], dict[str, Any]]] = []

    def recorder(name: str):
        def record(workspace: Any, *args: Any, **kwargs: Any) -> Any:
            calls.append((name, args, kwargs))
            return {"operation": name}
        return record

    for name in ("merge_preview", "merge_apply", "merge_atomic_preview", "merge_atomic"):
        monkeypatch.setattr(gateway_module.operations, name, recorder(name))

    assert _call(control, "merge_preview", source="feature", target="main") == (200, {"operation": "merge_preview"})
    _call(control, "merge_apply", source="feature", target="main", policy="api-default")
    _call(control, "merge_atomic_preview", source="feature", target="main", policy="snapshot_isolation")
    _call(control, "merge_atomic", source="feature", target="main", operation_id="op-1", preview_token="t")
    assert _call(control, "merge_atomic", source="feature", target="main")[0] == 400
    assert calls == [
        ("merge_preview", ("feature", "main", "abort_on_conflict"), {}),
        ("merge_apply", ("feature", "main", None), {}),
        ("merge_atomic_preview", ("feature", "main", "snapshot_isolation"), {}),
        ("merge_atomic", ("feature", "main"),
         {"operation_id": "op-1", "preview_token": "t", "policy": "abort_on_conflict"}),
    ]


def test_errors_are_redacted_and_internal_failures_are_500(tmp_path: Path, monkeypatch, capsys) -> None:
    secret = "postgresql://user:hunter2@db.internal/chronos_metadata"
    control = _control(tmp_path, metadata_url=f"sqlite:///{tmp_path / 'metadata.sqlite'}")
    control._workspaces["local"].urls += (secret,)

    def fail(*args: Any) -> Any:
        raise RuntimeError(f"cannot reach {secret}")

    def crash(*args: Any) -> Any:
        raise OSError("hunter2 disk failure")

    monkeypatch.setattr(gateway_module.operations, "diff", fail)
    monkeypatch.setattr(gateway_module.operations, "list_branches", crash)
    monkeypatch.setattr(GatewayControl, "_open", staticmethod(lambda entry: None))
    status, body = _call(control, "diff", left="main", right="feature")
    assert status == 400 and body == {"error": "cannot reach <database>"}
    status, body = _call(control, "list_branches")
    assert status == 500 and "hunter2" not in body["error"]
    assert "hunter2" not in capsys.readouterr().err


def test_workspace_opens_once_with_branch_local_schema(control: GatewayControl, monkeypatch) -> None:
    opened: list[bool] = []
    original = gateway_module.GatewayControl._open

    def counting(entry: Any) -> Any:
        if entry.context is None:
            opened.append(True)
        return original(entry)

    monkeypatch.setattr(GatewayControl, "_open", staticmethod(counting))
    _call(control, "list_branches")
    _call(control, "list_branches")
    assert opened == [True]
    context = control._workspaces["local"].context
    assert context.stores["app"]._backend.enable_schema_branching is True
    assert context.atomic_merge_enabled


def test_arguments_are_checked_before_the_workspace_opens(tmp_path: Path) -> None:
    control = GatewayControl({"workspaces": {"w": {"stores": {"app": {
        "data_url": "postgresql://user:secret@127.0.0.1:1/none"}}}}})
    assert _call(control, "list_branches", workspace="w", store="nope")[0] == 404
    assert _call(control, "diff", workspace="w", left="main")[0] == 400
    assert control._workspaces["w"].context is None


def test_multi_store_merges_need_merge_tables(tmp_path: Path) -> None:
    metadata = f"sqlite:///{tmp_path / 'metadata.sqlite'}"
    stores = {
        name: {"data_url": f"sqlite:///{tmp_path / name}.sqlite", "metadata_url": metadata}
        for name in ("one", "two")
    }
    control = GatewayControl({"workspaces": {"w": {"stores": stores}}})
    status, body = _call(control, "diff", workspace="w", left="main", right="feature")
    assert status == 400 and "merge_tables" in body["error"]


@pytest.mark.xfail(strict=True, reason=(
    "The gateway's stores keep metadata in a separate database, and the Python API does not yet "
    "open split stores with branch-local schema (ChronosBranchContext.from_database_adapter)."))
def test_split_metadata_store_as_the_gateway_configures_it(tmp_path: Path) -> None:
    control = _control(tmp_path, metadata_url=f"sqlite:///{tmp_path / 'metadata.sqlite'}")
    try:
        assert _call(control, "list_branches") == (200, ["main"])
    finally:
        control.close()


def _shared_control(tmp_path: Path) -> GatewayControl:
    entry = {"data_url": f"sqlite:///{tmp_path / 'shared.sqlite'}"}
    workspace = {"schema_branching": False, "stores": {"app": entry}}
    return GatewayControl(json.dumps({"workspaces": {"shared": workspace}}))


def test_shared_structure_ddl_creates_and_registers_for_every_branch(tmp_path: Path) -> None:
    control = _shared_control(tmp_path)
    try:
        ddl = partial(_call, control, "apply_ddl", workspace="shared", store="app")
        create = 'CREATE TABLE IF NOT EXISTS "hb_note" ("objectId" TEXT NOT NULL, name TEXT, PRIMARY KEY ("objectId"))'
        assert ddl(sql=create) == (200, {"tag": "CREATE TABLE", "created": True})
        assert ddl(sql=create) == (200, {"tag": "CREATE TABLE", "created": False})
        status, body = ddl(sql='CREATE TABLE "hb_note" ("objectId" TEXT PRIMARY KEY)')
        assert status == 400 and body["sqlstate"] == "42P07"
        # A partial index becomes a plain index on the same columns.
        index = 'CREATE INDEX IF NOT EXISTS hb_note_name ON "hb_note" (name) WHERE name IS NOT NULL'
        assert ddl(sql=index) == (200, {"tag": "CREATE INDEX", "created": True})
        assert ddl(sql=index) == (200, {"tag": "CREATE INDEX", "created": False})
        status, info = _call(control, "table_info", workspace="shared", store="app", table="hb_note")
        assert status == 200 and info["registered"] and info["primary_key"] == ["objectId"]
        assert [(item["name"], item["columns"]) for item in info["indexes"]] == [("hb_note_name", ["name"])]

        # The table exists on every branch, including ones created before it.
        workspace = control._open(control._workspaces["shared"])
        workspace.create_branch("agent")
        workspace.stores["app"].checkout("agent").execute(
            "INSERT INTO hb_note (\"objectId\", name) VALUES ('n1', 'note')")
        assert workspace.stores["app"].checkout("main").query("SELECT name FROM hb_note") == []
        assert workspace.stores["app"].checkout("agent").query("SELECT name FROM hb_note") == [{"name": "note"}]

        for sql, sqlstate in [
            ("CREATE TABLE keyless (id TEXT)", "0A000"),
            ("CREATE UNIQUE INDEX u ON hb_note (name)", "0A000"),
            ("CREATE INDEX ON hb_note (lower(name))", "0A000"),
            ("CREATE INDEX ON missing (id)", "42P01"),
            ("DROP TABLE hb_note", "0A000"),
            ("CREATE TABLE other.t (id TEXT PRIMARY KEY)", "0A000"),
        ]:
            status, body = ddl(sql=sql)
            assert status == 400 and body["sqlstate"] == sqlstate, (sql, body)
    finally:
        control.close()


def test_apply_ddl_is_refused_with_branch_local_schema(control: GatewayControl) -> None:
    status, body = _call(control, "apply_ddl", store="app", sql="CREATE TABLE t (id TEXT PRIMARY KEY)")
    assert status == 400 and "shared table structure" in body["error"]


@pytest.fixture
def postgres_url() -> Any:
    """A fresh PostgreSQL database, when CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL names a server."""
    import os
    from urllib.parse import urlsplit, urlunsplit
    from uuid import uuid4

    admin_url = os.environ.get("CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL")
    if not admin_url:
        pytest.skip("Set CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL to test PostgreSQL schemas")
    psycopg = pytest.importorskip("psycopg")
    name = f"chronos_schema_test_{uuid4().hex}"
    with psycopg.connect(admin_url, autocommit=True) as admin:
        admin.execute(f'CREATE DATABASE "{name}"')
        try:
            yield urlunsplit(urlsplit(admin_url)._replace(path=f"/{name}"))
        finally:
            admin.execute(f'DROP DATABASE "{name}" WITH (FORCE)')


def test_shared_structure_schemas_and_search_path(postgres_url: str) -> None:
    workspace = {"schema_branching": False, "stores": {"app": {"data_url": postgres_url}}}
    control = GatewayControl(json.dumps({"workspaces": {"shared": workspace}}))
    try:
        ddl = partial(_call, control, "apply_ddl", workspace="shared", store="app")
        assert ddl(sql='CREATE SCHEMA "ws1"') == (200, {"tag": "CREATE SCHEMA", "created": True})
        assert ddl(sql='CREATE SCHEMA IF NOT EXISTS "ws1"') == (200, {"tag": "CREATE SCHEMA", "created": False})
        status, body = ddl(sql='CREATE SCHEMA "ws1"')
        assert status == 400 and body["sqlstate"] == "42P06"

        # An explicitly qualified table, and an unqualified one placed by search_path.
        assert ddl(sql='CREATE TABLE "ws1"."heavenbase_backend" ("provider" TEXT PRIMARY KEY)')[0] == 200
        assert ddl(sql='CREATE TABLE notes (id TEXT PRIMARY KEY, body TEXT)', search_path=["ws1", "public"])[0] == 200
        assert ddl(sql='CREATE INDEX notes_body ON notes (body)', search_path=["ws1"])[0] == 200
        for table in ("ws1.heavenbase_backend", "ws1.notes"):
            status, info = _call(control, "table_info", workspace="shared", store="app", table=table)
            assert status == 200 and info["registered"], (table, info)
        _, info = _call(control, "table_info", workspace="shared", store="app", table="ws1.notes")
        assert [index["name"] for index in info["indexes"]] == ["ws1.notes_body"]
        # The same index name in another schema is a different index, as in PostgreSQL.
        assert ddl(sql='CREATE SCHEMA ws2')[0] == 200
        assert ddl(sql='CREATE TABLE notes (id TEXT PRIMARY KEY, body TEXT)', search_path=["ws2"])[0] == 200
        assert ddl(sql='CREATE INDEX notes_body ON notes (body)', search_path=["ws2"]) == (
            200, {"tag": "CREATE INDEX", "created": True})
        assert not _call(control, "table_info", workspace="shared", store="app", table="notes")[1]["registered"]
    finally:
        control.close()
