from __future__ import annotations

import json
from typing import Any, Mapping

import pytest

from attemory.client import AttemoryClient
from attemory.exceptions import AttemoryHTTPError, AttemoryResponseError
from attemory.transport import HTTPResponse


class FakeTransport:
    def __init__(self, responses: list[HTTPResponse]) -> None:
        self.responses = responses
        self.requests: list[dict[str, Any]] = []

    def request(
        self,
        *,
        host: str,
        port: int,
        timeout: float,
        method: str,
        path: str,
        body: bytes | None = None,
        headers: Mapping[str, str] | None = None,
    ) -> HTTPResponse:
        self.requests.append(
            {
                "host": host,
                "port": port,
                "timeout": timeout,
                "method": method,
                "path": path,
                "body": body,
                "headers": dict(headers or {}),
            }
        )
        return self.responses.pop(0)


def response(status_code: int, body: Any) -> HTTPResponse:
    return HTTPResponse(status_code=status_code, body=body, headers={})


def test_search_posts_json_and_quotes_session_path() -> None:
    transport = FakeTransport(
        [
            response(
                200,
                {
                    "data": {
                        "results": [
                            {
                                "rank": 1,
                                "id": "m1",
                                "memory_idx": 7,
                                "segment_id": 2,
                                "text": "memory text",
                            }
                        ]
                    }
                },
            )
        ]
    )
    client = AttemoryClient(host="example.test", port=1234, session_id="project/a")
    client._transport = transport

    results = client.search("find it", query_context="context", top_k=5)

    assert len(results) == 1
    assert results[0].id == "m1"
    request = transport.requests[0]
    assert request["method"] == "POST"
    assert request["path"] == "/v1/sessions/project%2Fa/search"
    assert request["headers"]["Content-Type"] == "application/json; charset=utf-8"
    assert json.loads(request["body"].decode("utf-8")) == {
        "query": "find it",
        "query_context": "context",
        "top_k": 5,
    }


def test_add_memory_allows_call_level_id_override() -> None:
    transport = FakeTransport(
        [
            response(
                200,
                {
                    "data": {
                        "prefill_tokens": 4,
                        "ctx_length": 10,
                        "remaining_tokens": 6,
                        "segment_id": 0,
                        "segment_count": 1,
                        "memory_idx": 0,
                        "id": "override",
                    }
                },
            )
        ]
    )
    client = AttemoryClient(session_id="demo")
    client._transport = transport

    usage = client.add_memory({"id": "original", "text": "hello"}, id="override")

    assert usage.id == "override"
    assert json.loads(transport.requests[0]["body"].decode("utf-8")) == {
        "id": "override",
        "text": "hello",
    }


def test_http_error_preserves_server_error_payload() -> None:
    transport = FakeTransport(
        [
            response(
                500,
                {
                    "error": {
                        "code": "INTERNAL_ERROR",
                        "message": "disk failed",
                        "details": {"path": "/tmp/cache"},
                    }
                },
            )
        ]
    )
    client = AttemoryClient(session_id="demo")
    client._transport = transport

    with pytest.raises(AttemoryHTTPError) as exc_info:
        client.index_session()

    assert exc_info.value.status_code == 500
    assert exc_info.value.error_code == "INTERNAL_ERROR"
    assert exc_info.value.error_message == "disk failed"
    assert exc_info.value.details == {"path": "/tmp/cache"}


def test_success_response_must_be_data_envelope() -> None:
    transport = FakeTransport([response(200, {"status": "ok"})])
    client = AttemoryClient()
    client._transport = transport

    with pytest.raises(AttemoryResponseError, match="missing data"):
        client.health()


def test_session_id_is_required_when_not_bound() -> None:
    client = AttemoryClient()

    with pytest.raises(ValueError, match="session_id is required"):
        client.create_session()
