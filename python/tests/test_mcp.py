from __future__ import annotations

from typing import Any

import pytest

from attemory.exceptions import AttemoryHTTPError
from attemory.mcp import AttemoryMCPService


class FakeClient:
    def __init__(self, restore_error: Exception | None) -> None:
        self.restore_error = restore_error
        self.calls: list[tuple[str, str]] = []

    def restore_session(self, *, session_id: str) -> dict[str, Any]:
        self.calls.append(("restore_session", session_id))
        if self.restore_error is not None:
            raise self.restore_error
        return {}

    def create_session(self, *, session_id: str) -> dict[str, Any]:
        self.calls.append(("create_session", session_id))
        return {}

    def add_system(self, text: str, *, session_id: str) -> dict[str, Any]:
        self.calls.append(("add_system", session_id))
        return {}


def http_error(status_code: int, code: str) -> AttemoryHTTPError:
    return AttemoryHTTPError(
        status_code=status_code,
        method="POST",
        path="/v1/sessions/demo/restore",
        response={"error": {"code": code, "message": code.lower()}},
    )


def service_with(client: FakeClient, *, auto_create_sessions: bool) -> AttemoryMCPService:
    return AttemoryMCPService(
        client=client,  # type: ignore[arg-type]
        system_prompt="system prompt",
        auto_create_sessions=auto_create_sessions,
        auto_index=False,
        auto_save=False,
        search_waits_for_index=False,
    )


def test_mcp_auto_create_creates_missing_session() -> None:
    client = FakeClient(http_error(404, "SESSION_NOT_FOUND"))
    service = service_with(client, auto_create_sessions=True)

    service._ensure_session_loaded("demo")

    assert client.calls == [
        ("restore_session", "demo"),
        ("create_session", "demo"),
        ("add_system", "demo"),
    ]


def test_mcp_auto_create_disabled_reraises_missing_session() -> None:
    client = FakeClient(http_error(404, "SESSION_NOT_FOUND"))
    service = service_with(client, auto_create_sessions=False)

    with pytest.raises(AttemoryHTTPError):
        service._ensure_session_loaded("demo")

    assert client.calls == [("restore_session", "demo")]


@pytest.mark.xfail(
    reason="Current MCP service auto-creates after any AttemoryError; it should only handle missing sessions.",
)
def test_mcp_auto_create_should_not_swallow_internal_errors() -> None:
    client = FakeClient(http_error(500, "INTERNAL_ERROR"))
    service = service_with(client, auto_create_sessions=True)

    with pytest.raises(AttemoryHTTPError):
        service._ensure_session_loaded("demo")
