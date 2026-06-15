from __future__ import annotations

from typing import Any


class AttemoryError(RuntimeError):
    """Base exception for attemory Python client errors."""


class AttemoryResponseError(AttemoryError):
    """Raised when the server returns malformed data for the requested operation."""


class AttemoryHTTPError(AttemoryError):
    """Raised for non-2xx HTTP responses."""

    def __init__(
        self,
        *,
        status_code: int,
        method: str,
        path: str,
        response: Any,
    ) -> None:
        self.status_code = status_code
        self.method = method
        self.path = path
        self.response = response
        self.error_code: str | None = None
        self.error_message: str | None = None
        self.details: Any = None

        message = f"{method} {path} failed with HTTP {status_code}"
        if isinstance(response, dict) and isinstance(response.get("error"), dict):
            error = response["error"]
            raw_code = error.get("code")
            raw_message = error.get("message")
            if isinstance(raw_code, str):
                self.error_code = raw_code
            if isinstance(raw_message, str):
                self.error_message = raw_message
            self.details = error.get("details")
            if self.error_code and self.error_message:
                message += f": {self.error_code}: {self.error_message}"
            elif self.error_message:
                message += f": {self.error_message}"
            else:
                message += f": {error!r}"
        else:
            message += f": {response!r}"
        super().__init__(message)
