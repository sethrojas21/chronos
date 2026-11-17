"""Remove configured credentials from error messages before reporting them."""
from __future__ import annotations

import re
from collections.abc import Iterable
from urllib.parse import parse_qsl, unquote, urlsplit


def redact_message(message: str, *, secrets: Iterable[str | None] = (), urls: Iterable[str | None] = ()) -> str:
    """Replace configured URLs, their passwords, and other secrets in a message."""
    sensitive = [secret for secret in secrets if secret]
    for url in urls:
        if url:
            message = message.replace(url, "<database>")
            try:
                parsed = urlsplit(url)
            except ValueError:
                continue
            if parsed.password:
                sensitive.extend((parsed.password, unquote(parsed.password)))
            sensitive.extend(value for key, value in parse_qsl(parsed.query) if key == "password" and value)
            for item in parsed.query.split("&"):
                key, separator, value = item.partition("=")
                if separator and unquote(key) == "password" and value:
                    sensitive.append(value)
    for secret in sorted(set(sensitive), key=len, reverse=True):
        message = message.replace(secret, "<redacted>")
    message = re.sub(r"([a-zA-Z][\w+.-]*://)[^\s/@]+@", r"\1<credentials>@", message)
    return re.sub(r"(?i)(password\s*=\s*)(?:'[^']*'|\"[^\"]*\"|[^\s]+)", r"\1<redacted>", message)
