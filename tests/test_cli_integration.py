"""Real CLI processes exercising the same workflows on SQLite and PostgreSQL."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from functools import partial
from pathlib import Path
from typing import Any, Iterator
from urllib.parse import urlsplit, urlunsplit
from uuid import uuid4

import psycopg
import pytest
from psycopg import sql as postgres_sql

from chronos_core.branching import ChronosBranchContext


def _invoke_cli(
    database_url: str, *args: str, ok: bool = True, input_text: str | None = None,
) -> Any:
    result = subprocess.run(
        [sys.executable, "-X", "faulthandler", "-m", "chronos_core.cli",
         "--database-url", database_url, *args],
        input=input_text, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == (0 if ok else 1), result.stderr
    if not ok:
        assert result.stdout == ""
        assert "Traceback" not in result.stderr
    return json.loads(result.stdout) if result.stdout.strip() else None


@pytest.fixture(scope="session")
def postgres_cli_database_url() -> Iterator[str]:
    """Create one owned database for the run, without resetting the admin database."""
    admin_url = os.environ.get("CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL")
    if not admin_url:
        pytest.skip("Set CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL to test PostgreSQL")
    parsed = urlsplit(admin_url)
    if parsed.scheme not in {"postgres", "postgresql"}:
        pytest.fail("CHRONOS_CLI_TEST_POSTGRES_ADMIN_URL must be a PostgreSQL URL")
    name = f"chronos_cli_test_{uuid4().hex}"
    with psycopg.connect(admin_url, autocommit=True, connect_timeout=10) as admin:
        admin.execute(postgres_sql.SQL("CREATE DATABASE {}").format(postgres_sql.Identifier(name)))
        try:
            yield urlunsplit(parsed._replace(path=f"/{name}"))
        finally:
            admin.execute(
                postgres_sql.SQL("DROP DATABASE {} WITH (FORCE)").format(postgres_sql.Identifier(name))
            )


@pytest.fixture(params=["sqlite", "postgres"])
def cli_database_url(request: pytest.FixtureRequest, tmp_path: Path) -> str:
    if request.param == "sqlite":
        return f"sqlite:///{tmp_path / 'workflow.sqlite'}"
    url = request.getfixturevalue("postgres_cli_database_url")
    # This URL belongs only to the database created by our session fixture.
    with psycopg.connect(url, autocommit=True, connect_timeout=10) as database:
        database.execute("DROP SCHEMA public CASCADE")
        database.execute("CREATE SCHEMA public")
    return url


def test_relational_workflow(cli_database_url: str) -> None:
    url = cli_database_url
    database = ChronosBranchContext.connect(url)
    try:
        database.db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)")
        database.db.commit()
    finally:
        database.close()

    invoke = partial(_invoke_cli, url)

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


def test_cli_only_setup(cli_database_url: str) -> None:
    url = cli_database_url

    invoke = partial(_invoke_cli, url)

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


def test_relational_sql_inputs_and_rollback(cli_database_url: str, tmp_path: Path) -> None:
    invoke = partial(_invoke_cli, cli_database_url)
    statement = tmp_path / "statement.sql"
    statement.write_text(
        "CREATE TABLE items (id INTEGER PRIMARY KEY, value TEXT)", encoding="utf-8",
    )
    invoke("table", "create", "--file", str(statement))
    invoke("table", "register", "items", "--primary-key", "id")
    statement.write_text(
        "INSERT INTO items (id, value) VALUES (:id, :value)", encoding="utf-8",
    )
    assert invoke("execute", "--file", str(statement), "--params",
                  '{"id": 1, "value": "100% done"}') == {"rowcount": 1}
    assert invoke("query", "--stdin", "--params", '{"pattern": "100%"}',
                  input_text="SELECT * FROM items WHERE value LIKE :pattern") == [
        {"id": 1, "value": "100% done"},
    ]
    assert invoke("execute", "--stdin", "--params", '{"id": 1, "value": "updated"}',
                  input_text="UPDATE items SET value = :value WHERE id = :id") == {"rowcount": 1}
    statement.write_text("SELECT * FROM items", encoding="utf-8")
    assert invoke("query", "--file", str(statement)) == [{"id": 1, "value": "updated"}]
    # A failed statement must not leave its earlier rows committed.
    invoke("execute", "INSERT INTO items (id, value) VALUES (2, 'partial'), (1, 'duplicate')", ok=False)
    assert invoke("query", input_text="SELECT * FROM items ORDER BY id") == [
        {"id": 1, "value": "updated"},
    ]

