from __future__ import annotations

import json
from dataclasses import dataclass
from http.client import HTTPConnection
from typing import Any, Mapping

from .exceptions import AttemoryResponseError


@dataclass
class HTTPResponse:
    status_code: int
    body: Any
    headers: Mapping[str, str]


class HTTPTransport:
    """Small stdlib transport wrapper used by AttemoryClient."""

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
        conn = HTTPConnection(host, port, timeout=timeout)
        try:
            conn.request(method, path, body=body, headers=dict(headers or {}))
            response = conn.getresponse()
            raw_body = response.read()
            response_headers = {key.lower(): value for key, value in response.getheaders()}
            parsed_body = _decode_body(raw_body)
            return HTTPResponse(
                status_code=response.status,
                body=parsed_body,
                headers=response_headers,
            )
        finally:
            conn.close()


def _decode_body(raw_body: bytes) -> Any:
    if not raw_body:
        return None
    try:
        text = raw_body.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise AttemoryResponseError("response body is not valid UTF-8") from exc
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise AttemoryResponseError("response body is not valid JSON") from exc
