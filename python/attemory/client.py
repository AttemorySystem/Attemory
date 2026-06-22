from __future__ import annotations

import json
from typing import Any, Iterable, Mapping
from urllib.parse import quote

from .exceptions import AttemoryHTTPError, AttemoryResponseError
from .models import MemoryInput, SearchResult, SessionStatus, TokenUsage
from .transport import HTTPTransport


DEFAULT_TIMEOUT_SECONDS = 3600.0
DEFAULT_SYSTEM_PROMPT = (
    "Read the following memories carefully and find the most relevant memory to the query."
)


class AttemoryClient:
    """High-level HTTP client for the attemory server."""

    def __init__(
        self,
        *,
        host: str = "127.0.0.1",
        port: int = 9006,
        session_id: str | None = None,
        timeout: float = DEFAULT_TIMEOUT_SECONDS,
    ) -> None:
        self.host = host
        self.port = port
        self.session_id = session_id
        self.timeout = timeout
        self._transport = HTTPTransport()

    def with_session(self, session_id: str) -> AttemoryClient:
        return AttemoryClient(
            host=self.host,
            port=self.port,
            session_id=session_id,
            timeout=self.timeout,
        )

    def health(self) -> bool:
        body = self._request_json("GET", "/health")
        return isinstance(body, Mapping) and body.get("status") == "ok"

    def list_sessions(self) -> list[SessionStatus]:
        body = self._request_json("GET", "/v1/sessions")
        if not isinstance(body, Mapping):
            raise AttemoryResponseError("session list response must be a JSON object")
        sessions = body.get("sessions", [])
        if not isinstance(sessions, list):
            raise AttemoryResponseError("session list field sessions must be an array")
        parsed: list[SessionStatus] = []
        for index, item in enumerate(sessions):
            if not isinstance(item, Mapping):
                raise AttemoryResponseError(f"session list item {index} must be a JSON object")
            try:
                parsed.append(SessionStatus.from_json(item))
            except (TypeError, ValueError) as exc:
                raise AttemoryResponseError(f"session list item {index} is malformed: {exc}") from exc
        return parsed

    def create_session(
        self,
        session_id: str | None = None,
        *,
        kv_persist: bool = False,
    ) -> TokenUsage:
        body = {"kv_persist": True} if kv_persist else None
        return self._parse_token_usage(
            self._session_command("POST", json_body=body, session_id=session_id)
        )

    def delete_session(self, session_id: str | None = None) -> dict[str, Any]:
        return self._expect_mapping(self._session_command("DELETE", session_id=session_id))

    def add_system(self, text: str, session_id: str | None = None) -> TokenUsage:
        body = self._session_command(
            "POST",
            "system",
            json_body={"text": text},
            session_id=session_id,
        )
        return self._parse_token_usage(body)

    def add_memory(
        self,
        memory: MemoryInput | Mapping[str, Any] | str,
        session_id: str | None = None,
        *,
        id: str | None = None,
    ) -> TokenUsage:
        normalized = MemoryInput.from_value(memory, 0)
        request = normalized.to_request_json()
        if id is not None:
            request["id"] = id
        body = self._session_command(
            "POST",
            "memories",
            json_body=request,
            session_id=session_id,
        )
        return self._parse_token_usage(body)

    def next_segment(self, session_id: str | None = None) -> dict[str, Any]:
        body = self._session_command("POST", "segments/next", session_id=session_id)
        return self._expect_mapping(body)

    def index_session(self, session_id: str | None = None) -> dict[str, Any]:
        return self._expect_mapping(self._session_command("POST", "index", session_id=session_id))

    def save_session(self, session_id: str | None = None) -> dict[str, Any]:
        return self._expect_mapping(self._session_command("POST", "save", session_id=session_id))

    def restore_session(self, session_id: str | None = None) -> dict[str, Any]:
        return self._expect_mapping(self._session_command("POST", "restore", session_id=session_id))

    def clear_cache(self, session_id: str | None = None) -> dict[str, Any]:
        body = self._session_command("POST", "clear-cache", session_id=session_id)
        return self._expect_mapping(body)

    def search(
        self,
        query: str,
        *,
        session_id: str | None = None,
        query_context: str | None = None,
        top_k: int | None = None,
    ) -> list[SearchResult]:
        request: dict[str, Any] = {"query": query}
        if query_context is not None:
            request["query_context"] = query_context
        if top_k is not None:
            request["top_k"] = top_k
        body = self._session_command(
            "POST",
            "search",
            json_body=request,
            session_id=session_id,
        )
        return _parse_search_results(body)

    def oneshot_search(
        self,
        query: str,
        memories: Iterable[MemoryInput | Mapping[str, Any] | str],
        *,
        system: str = DEFAULT_SYSTEM_PROMPT,
        query_context: str | None = None,
        top_k: int | None = None,
    ) -> list[SearchResult]:
        normalized = _normalize_memories(memories)
        body: dict[str, Any] = {
            "system": system,
            "query": query,
            "memories": [memory.to_request_json() for memory in normalized],
        }
        if query_context is not None:
            body["query_context"] = query_context
        if top_k is not None:
            body["top_k"] = top_k

        response = self._request_json(
            "POST",
            "/v1/oneshot/search",
            json_body=body,
        )
        return _parse_search_results(response)

    def _session_command(
        self,
        method: str,
        command: str | None = None,
        *,
        json_body: Mapping[str, Any] | None = None,
        session_id: str | None = None,
    ) -> Any:
        path = _session_path(self._resolve_session_id(session_id), command)
        return self._request_json(method, path, json_body=json_body)

    def _request_json(
        self,
        method: str,
        path: str,
        *,
        json_body: Mapping[str, Any] | None = None,
    ) -> Any:
        headers = {"Accept": "application/json"}
        payload: bytes | None = None
        if json_body is not None:
            payload = json.dumps(
                json_body,
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"

        response = self._transport.request(
            host=self.host,
            port=self.port,
            timeout=self.timeout,
            method=method,
            path=path,
            body=payload,
            headers=headers,
        )
        if response.status_code >= 400:
            raise AttemoryHTTPError(
                status_code=response.status_code,
                method=method,
                path=path,
                response=response.body,
            )
        return _extract_response_data(response.body)

    def _resolve_session_id(self, session_id: str | None) -> str:
        resolved = session_id or self.session_id
        if not resolved:
            raise ValueError("session_id is required")
        return resolved

    @staticmethod
    def _expect_mapping(body: Any) -> dict[str, Any]:
        if not isinstance(body, Mapping):
            raise AttemoryResponseError("expected JSON object response")
        return dict(body)

    @staticmethod
    def _parse_token_usage(body: Any) -> TokenUsage:
        try:
            return TokenUsage.from_json(AttemoryClient._expect_mapping(body))
        except (TypeError, ValueError) as exc:
            raise AttemoryResponseError(f"token usage response is malformed: {exc}") from exc


def _session_path(session_id: str, command: str | None = None) -> str:
    base = f"/v1/sessions/{quote(session_id, safe='')}"
    return base if command is None else f"{base}/{command}"


def _normalize_memories(memories: Iterable[MemoryInput | Mapping[str, Any] | str]) -> list[MemoryInput]:
    return [MemoryInput.from_value(memory, index) for index, memory in enumerate(memories)]


def _parse_search_results(body: Any) -> list[SearchResult]:
    if not isinstance(body, Mapping):
        raise AttemoryResponseError("search response data must be a JSON object")
    results = body.get("results")
    if not isinstance(results, list):
        raise AttemoryResponseError("search response data field results must be an array")
    parsed: list[SearchResult] = []
    for index, item in enumerate(results):
        if not isinstance(item, Mapping):
            raise AttemoryResponseError(f"search result item {index} must be a JSON object")
        try:
            parsed.append(SearchResult.from_json(item))
        except (TypeError, ValueError) as exc:
            raise AttemoryResponseError(f"search result item {index} is malformed: {exc}") from exc
    return parsed


def _extract_response_data(body: Any) -> Any:
    if not isinstance(body, Mapping):
        raise AttemoryResponseError("response body must be a JSON object")
    if "error" in body:
        raise AttemoryResponseError("successful response body must not include error field")
    if "data" not in body:
        raise AttemoryResponseError("response body missing data field")
    return body["data"]
