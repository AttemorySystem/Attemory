from __future__ import annotations

import io
import json
from typing import Any

from attemory import cli
from attemory.exceptions import AttemoryHTTPError
from attemory.models import TokenUsage


class FakeClient:
    calls: list[tuple[str, tuple[Any, ...], dict[str, Any]]] = []

    def __init__(
        self,
        *,
        host: str,
        port: int,
        session_id: str | None,
        timeout: float,
    ) -> None:
        self.host = host
        self.port = port
        self.session_id = session_id
        self.timeout = timeout

    def add_memory(self, *args: Any, **kwargs: Any) -> TokenUsage:
        self.calls.append(("add_memory", args, kwargs))
        return TokenUsage(
            prefill_tokens=3,
            ctx_length=10,
            remaining_tokens=7,
            segment_id=0,
            segment_count=1,
            memory_idx=0,
            id=kwargs.get("id"),
        )


def test_cli_add_memory_reads_stdin_and_prints_data_envelope(
    monkeypatch: Any,
    capsys: Any,
) -> None:
    FakeClient.calls = []
    monkeypatch.setattr(cli, "AttemoryClient", FakeClient)
    monkeypatch.setattr(cli.sys, "stdin", io.StringIO("memory from stdin"))

    code = cli.main(["--session", "demo", "add-memory", "--id", "m1"])

    assert code == 0
    captured = capsys.readouterr()
    assert captured.err == ""
    payload = json.loads(captured.out)
    assert payload["data"]["id"] == "m1"
    assert FakeClient.calls == [
        ("add_memory", ("memory from stdin", "demo"), {"id": "m1"}),
    ]


def test_cli_error_envelope_prefers_http_error_fields() -> None:
    error = AttemoryHTTPError(
        status_code=404,
        method="POST",
        path="/v1/sessions/missing/search",
        response={
            "error": {
                "code": "SESSION_NOT_FOUND",
                "message": "session not found: missing",
                "details": {"session_id": "missing"},
            }
        },
    )

    envelope = cli.error_envelope(error)

    assert envelope == {
        "error": {
            "code": "SESSION_NOT_FOUND",
            "message": "session not found: missing",
            "details": {
                "error_type": "AttemoryHTTPError",
                "status_code": 404,
                "server": {"session_id": "missing"},
            },
        }
    }
