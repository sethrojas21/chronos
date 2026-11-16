"""JSON encoding of Chronos API results, shared by the CLI and the gateway."""
from __future__ import annotations

import base64
from dataclasses import asdict, is_dataclass
from datetime import date, datetime, time
from decimal import Decimal
from typing import Any


def json_default(value: Any) -> Any:
    """`json.dumps` default: dataclasses become objects, dates ISO strings,
    decimals strings, and bytes a tagged base64 object."""
    if is_dataclass(value) and not isinstance(value, type):
        return asdict(value)
    if isinstance(value, (datetime, date, time)):
        return value.isoformat()
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, bytes):
        return {"encoding": "base64", "data": base64.b64encode(value).decode("ascii")}
    raise TypeError(f"Unsupported JSON value type: {type(value).__name__}")
