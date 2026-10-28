from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest
from typer.testing import CliRunner

from chronos_core.branching import BranchingError, ChronosBranchContext
from chronos_core.cli.main import app

runner = CliRunner()


@pytest.fixture
def connection(monkeypatch: pytest.MonkeyPatch) -> MagicMock:
    database = MagicMock()
    database.list_branches.return_value = []
    database.checkout.return_value.__enter__.return_value.query.return_value = []
    monkeypatch.setattr(ChronosBranchContext, "connect", MagicMock(return_value=database))
    monkeypatch.delenv("CHRONOS_DATABASE_URL", raising=False)
    return database


def test_configuration_and_help(connection: MagicMock) -> None:
    for args in (["--help"], ["branch", "--help"], ["query", "--help"]):
        assert runner.invoke(app, args).exit_code == 0
    ChronosBranchContext.connect.assert_not_called()
    result = runner.invoke(app, ["branch", "list"])
    assert result.exit_code == 2
    ChronosBranchContext.connect.assert_not_called()
    result = runner.invoke(app, ["branch", "list"], env={"CHRONOS_DATABASE_URL": "sqlite:///env.db"})
    assert result.exit_code == 0
    ChronosBranchContext.connect.assert_called_with("sqlite:///env.db")
    result = runner.invoke(app, ["--database-url", "sqlite:///flag.db", "branch", "list"],
                           env={"CHRONOS_DATABASE_URL": "sqlite:///env.db"})
    assert result.exit_code == 0
    ChronosBranchContext.connect.assert_called_with("sqlite:///flag.db")
    assert json.loads(result.stdout) == []
    assert connection.close.call_count == 2


@pytest.mark.parametrize(("args", "method", "expected_args", "expected_kwargs"), [
    (["branch", "list"], "list_branches", (), {}),
    (["branch", "create", "trial"], "create_branch", ("trial",), {"from_branch": "main"}),
    (["branch", "create", "trial", "--from", "parent"], "create_branch", ("trial",), {"from_branch": "parent"}),
    (["branch", "delete", "trial"], "delete_branch", ("trial",), {}),
    (["table", "register", "items", "--primary-key", "id", "--primary-key", "tenant"], "register_table", ("items", ["id", "tenant"]), {}),
    (["checkpoint", "create", "saved", "--branch", "trial"], "create_checkpoint", ("saved",), {"branch": "trial"}),
    (["checkpoint", "list"], "list_checkpoints", (), {"branch": None}),
    (["checkpoint", "list", "--branch", "trial"], "list_checkpoints", (), {"branch": "trial"}),
    (["diff", "main", "trial"], "diff", ("main", "trial"), {}),
    (["diff", "main", "trial", "--table", "items"], "diff_rows", ("main", "trial", "items"), {}),
    (["merge", "preview", "trial", "main"], "merge_preview", ("trial", "main"), {"policy": "abort_on_conflict"}),
    (["merge", "apply", "trial", "main", "--policy", "snapshot_isolation"], "merge_apply", ("trial", "main"), {"policy": "snapshot_isolation"}),
])
def test_api_forwarding(connection: MagicMock, args: list[str], method: str,
                        expected_args: tuple, expected_kwargs: dict) -> None:
    getattr(connection, method).return_value = None
    result = runner.invoke(app, ["--database-url", "sqlite:///test.db", *args])
    assert result.exit_code == 0, result.output
    getattr(connection, method).assert_called_once_with(*expected_args, **expected_kwargs)
    assert result.stdout == ""
    connection.close.assert_called_once()


@pytest.mark.parametrize("command", ["query", "execute"])
@pytest.mark.parametrize("source", ["string", "file", "stdin", "pipe"])
def test_sql_sources(connection: MagicMock, tmp_path: Path, command: str, source: str) -> None:
    sql = "SELECT *\nFROM items WHERE id = :id\n"
    args = ["--database-url", "sqlite:///test.db", command, "--branch", "trial", "--params", '{"id": 3}']
    input_text = None
    if source == "string":
        args.append(sql)
    elif source == "file":
        path = tmp_path / "query.sql"
        path.write_text(sql, encoding="utf-8")
        args.extend(["--file", str(path)])
    else:
        input_text = sql
        if source == "stdin":
            args.append("--stdin")
    session = connection.checkout.return_value.__enter__.return_value
    getattr(session, command).return_value = [{"id": 3}] if command == "query" else {"rowcount": 1}
    result = runner.invoke(app, args, input=input_text)
    assert result.exit_code == 0, result.output
    connection.checkout.assert_called_once_with("trial")
    getattr(session, command).assert_called_once_with(sql, {"id": 3})
    assert json.loads(result.stdout) == getattr(session, command).return_value
    connection.checkout.return_value.__exit__.assert_called_once()
    connection.close.assert_called_once()


@pytest.mark.parametrize("args", [
    ["query"], ["query", " "], ["query", "SELECT 1", "--stdin"],
    ["query", "SELECT 1", "--file", "missing.sql"],
    ["query", "--file", "missing.sql"],
    ["query", "SELECT 1", "--params", "[]"],
    ["execute", "SELECT 1", "--params", "broken"],
    ["merge", "apply", "a", "b", "--policy", "unknown"],
    ["table", "register", "items"],
])
def test_bad_inputs_do_not_connect(connection: MagicMock, args: list[str]) -> None:
    result = runner.invoke(app, ["--database-url", "sqlite:///test.db", *args])
    assert result.exit_code == 2, result.output
    ChronosBranchContext.connect.assert_not_called()


def test_interactive_missing_sql(connection: MagicMock, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    from chronos_core.cli.main import _sql_input
    import typer
    with pytest.raises(typer.BadParameter, match="Provide SQL"):
        _sql_input(None, None, False)
    ChronosBranchContext.connect.assert_not_called()


def test_cleanup_and_redaction(connection: MagicMock) -> None:
    url = "postgresql://user:secret@localhost/db"
    session = connection.checkout.return_value.__enter__.return_value
    session.query.side_effect = BranchingError(f"Cannot use {url}; password=secret")
    result = runner.invoke(app, ["--database-url", url, "query", "SELECT 1"])
    assert result.exit_code == 1
    assert "secret" not in result.output
    assert "Error:" in result.stderr
    connection.checkout.return_value.__exit__.assert_called_once()
    connection.close.assert_called_once()


def test_connection_failure_does_not_close_unassigned_context(connection: MagicMock) -> None:
    ChronosBranchContext.connect.side_effect = ValueError("unsupported database")
    result = runner.invoke(app, ["--database-url", "bad", "branch", "list"])
    assert result.exit_code == 1
    assert "unsupported database" in result.stderr
    connection.close.assert_not_called()



def test_sqlite_workflow(tmp_path: Path) -> None:
    url = f"sqlite:///{tmp_path / 'workflow.db'}"
    database = ChronosBranchContext.connect(url)
    try:
        database.db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)")
        database.db.commit()
    finally:
        database.close()

    def invoke(*args: str, ok: bool = True):
        result = subprocess.run(
            [sys.executable, "-X", "faulthandler", "-m", "chronos_core.cli", "--database-url", url, *args],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == (0 if ok else 1), result.stderr
        return json.loads(result.stdout) if result.stdout.strip() else None

    assert invoke("table", "register", "items", "--primary-key", "id") is None
    assert invoke("branch", "create", "trial") is None
    invoke("branch", "create", "trial", ok=False)
    assert {branch["branch_id"] for branch in invoke("branch", "list")} == {"main", "trial"}
    assert invoke("execute", "INSERT INTO items (id, value) VALUES (:id, :value)", "--branch", "trial",
                  "--params", '{"id": 1, "value": "hello"}') == {"rowcount": 1}
    assert invoke("query", "SELECT * FROM items", "--branch", "trial") == [{"id": 1, "value": "hello"}]
    assert invoke("query", "SELECT * FROM items") == []
    assert invoke("checkpoint", "create", "saved", "--branch", "trial")["checkpoint_id"] == "saved"
    assert len(invoke("checkpoint", "list", "--branch", "trial")) == 1
    assert len(invoke("diff", "main", "trial")["changes"]) == 1
    assert len(invoke("diff", "main", "trial", "--table", "items")) == 1
    assert invoke("merge", "preview", "trial", "main")["conflicts"] == []
    assert invoke("merge", "apply", "trial", "main")["applied"] == 1
    assert invoke("query", "SELECT * FROM items") == [{"id": 1, "value": "hello"}]
    invoke("branch", "create", "conflicting")
    invoke("execute", "UPDATE items SET value = 'source' WHERE id = 1", "--branch", "conflicting")
    invoke("execute", "UPDATE items SET value = 'target' WHERE id = 1")
    assert len(invoke("merge", "preview", "conflicting", "main")["conflicts"]) == 1
    invoke("merge", "apply", "conflicting", "main", ok=False)
    invoke("merge", "apply", "conflicting", "main", "--policy", "snapshot_isolation", ok=False)
    assert invoke("query", "SELECT value FROM items") == [{"value": "target"}]
    invoke("branch", "delete", "conflicting")
    invoke("query", "SELECT * FROM items", "--branch", "missing", ok=False)
    invoke("execute", "DROP TABLE items", ok=False)
    assert invoke("branch", "delete", "trial") is None
    assert [branch["branch_id"] for branch in invoke("branch", "list")] == ["main"]


def test_installed_entrypoints() -> None:
    executable = Path(sys.executable).parent / "chronos"
    for args in ([str(executable), "--help"], [sys.executable, "-m", "chronos_core.cli", "--help"]):
        env = dict(os.environ)
        env.pop("CHRONOS_DATABASE_URL", None)
        result = subprocess.run(args, capture_output=True, text=True, env=env, timeout=30)
        assert result.returncode == 0, result.stderr
        assert "--database-url" in result.stdout


def test_create_table_forwarding(connection: MagicMock) -> None:
    connection.db.commit.return_value = None
    statement = "CREATE TABLE items (id INTEGER PRIMARY KEY)"
    result = runner.invoke(app, ["--database-url", "sqlite:///test.db", "table", "create", statement])
    assert result.exit_code == 0, result.output
    connection.db.execute.assert_called_once_with(statement)
    connection.db.commit.assert_called_once()
    connection.checkout.assert_not_called()
    assert result.stdout == ""


@pytest.mark.parametrize("statement", [
    "DELETE FROM items", "CREATE INDEX idx ON items(id)",
    "CREATE TABLE items (id INTEGER); DROP TABLE items", "not sql",
])
def test_create_table_rejects_other_sql(connection: MagicMock, statement: str) -> None:
    result = runner.invoke(app, ["--database-url", "sqlite:///test.db", "table", "create", statement])
    assert result.exit_code == 2, result.output
    ChronosBranchContext.connect.assert_not_called()


def test_native_error_is_reported_without_traceback(connection: MagicMock) -> None:
    session = connection.checkout.return_value.__enter__.return_value
    session.execute.side_effect = RuntimeError("INSERT must specify a column list")
    result = runner.invoke(app, [
        "--database-url", "sqlite:///test.db", "execute", "INSERT INTO items VALUES (1)",
    ])
    assert result.exit_code == 1
    assert result.stdout == ""
    assert result.stderr.strip() == "Error: INSERT must specify a column list"
    connection.checkout.return_value.__exit__.assert_called_once()
    connection.close.assert_called_once()


def test_cli_only_sqlite_setup(tmp_path: Path) -> None:
    url = f"sqlite:///{tmp_path / 'cli-only.sqlite'}"

    def invoke(*args: str):
        result = subprocess.run(
            [sys.executable, "-X", "faulthandler", "-m", "chronos_core.cli", "--database-url", url, *args],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout) if result.stdout.strip() else None

    assert invoke("table", "create",
                  "CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)") is None
    invoke("table", "register", "items", "--primary-key", "id")
    invoke("branch", "create", "experiment")
    assert invoke("execute", "INSERT INTO items (id, value) VALUES (1, 'hello')",
                  "--branch", "experiment") == {"rowcount": 1}
    assert invoke("query", "SELECT * FROM items", "--branch", "experiment") == [{"id": 1, "value": "hello"}]
    assert invoke("query", "SELECT * FROM items") == []
    assert len(invoke("diff", "main", "experiment")["changes"]) == 1
    assert invoke("merge", "apply", "experiment", "main")["applied"] == 1
    assert invoke("query", "SELECT * FROM items") == [{"id": 1, "value": "hello"}]
