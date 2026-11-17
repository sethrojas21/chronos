"""The gateway's Python control entry point, against SQLite stores without the gateway."""
from __future__ import annotations

import json
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
