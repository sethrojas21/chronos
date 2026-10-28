from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import date, datetime, time
from decimal import Decimal
from unittest.mock import MagicMock

import pytest
import typer

from chronos_core.branching import ChronosBranchContext
from chronos_core.cli._support import DatabaseConfig, connect, print_result


def test_json_values(capsys: pytest.CaptureFixture[str]) -> None:
    @dataclass
    class Result:
        values: list
    print_result(Result([date(2026, 1, 2), datetime(2026, 1, 2, 3, 4), time(3, 4), Decimal("1.20"), b"hi"]))
    assert json.loads(capsys.readouterr().out) == {"values": [
        "2026-01-02", "2026-01-02T03:04:00", "03:04:00", "1.20", {"encoding": "base64", "data": "aGk="}
    ]}
    print_result(None)
    assert capsys.readouterr().out == ""
    with pytest.raises(TypeError, match="Unsupported JSON value"):
        print_result(object())


@pytest.mark.parametrize("failure", [False, True])
def test_connection_cleanup(monkeypatch: pytest.MonkeyPatch, failure: bool) -> None:
    database = MagicMock()
    monkeypatch.setattr(ChronosBranchContext, "connect", MagicMock(return_value=database))
    if failure:
        with pytest.raises(typer.Exit):
            with connect(DatabaseConfig("sqlite:///:memory:")):
                raise ValueError("operation failed")
    else:
        with connect(DatabaseConfig("sqlite:///:memory:")) as context:
            assert context is database
    database.close.assert_called_once()
