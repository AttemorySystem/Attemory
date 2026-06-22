from __future__ import annotations

from dataclasses import asdict, dataclass, is_dataclass
from typing import Any, Mapping


JsonDict = dict[str, Any]
_MEMORY_INPUT_FIELDS = {"id", "text"}


def _as_int(value: Any, *, default: int = 0) -> int:
    if value is None:
        return default
    if isinstance(value, bool):
        raise TypeError("expected integer-compatible value, got bool")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        return int(value)
    raise TypeError(f"expected integer-compatible value, got {type(value).__name__}")


def _optional_int(value: Any) -> int | None:
    if value is None:
        return None
    return _as_int(value)


def _as_bool(value: Any, *, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    raise TypeError(f"expected boolean value, got {type(value).__name__}")


@dataclass
class MemoryInput:
    """Memory input for adding memories or one-shot search."""

    text: str = ""
    id: str | None = None

    @classmethod
    def from_value(cls, value: MemoryInput | Mapping[str, Any] | str, index: int) -> MemoryInput:
        if isinstance(value, MemoryInput):
            memory = value
        elif isinstance(value, str):
            memory = cls(text=value)
        elif isinstance(value, Mapping):
            unknown_fields = set(value) - _MEMORY_INPUT_FIELDS
            if unknown_fields:
                field = sorted(str(key) for key in unknown_fields)[0]
                raise TypeError(f"unsupported memory field: {field}")
            text = value.get("text", "")
            if not isinstance(text, str):
                raise TypeError("memory field text must be a string")
            raw_id = value.get("id")
            if raw_id is not None and not isinstance(raw_id, str):
                raise TypeError("memory field id must be a string")
            memory = cls(text=text, id=raw_id)
        else:
            raise TypeError(f"unsupported memory value: {type(value).__name__}")

        return memory

    def to_request_json(self) -> JsonDict:
        body = {"text": self.text}
        if self.id is not None:
            body["id"] = self.id
        return body


@dataclass
class TokenUsage:
    prefill_tokens: int
    ctx_length: int
    remaining_tokens: int
    segment_id: int
    segment_count: int
    memory_idx: int | None = None
    id: str | None = None

    @classmethod
    def from_json(cls, data: Mapping[str, Any]) -> TokenUsage:
        return cls(
            prefill_tokens=_as_int(data.get("prefill_tokens")),
            ctx_length=_as_int(data.get("ctx_length")),
            remaining_tokens=_as_int(data.get("remaining_tokens")),
            segment_id=_as_int(data.get("segment_id")),
            segment_count=_as_int(data.get("segment_count")),
            memory_idx=_optional_int(data.get("memory_idx")),
            id=data.get("id") if isinstance(data.get("id"), str) else None,
        )


@dataclass
class SessionStatus:
    session_id: str
    memory_count: int
    segment_count: int
    total_tokens: int
    resident_segments: int
    indexed_segments: int
    saved_segments: int
    indexed: bool
    disk_cached: bool
    plan_ready: bool
    facts_dirty: bool
    kv_persist: bool = False

    @classmethod
    def from_json(cls, data: Mapping[str, Any]) -> SessionStatus:
        return cls(
            session_id=str(data.get("session_id", "")),
            memory_count=_as_int(data.get("memory_count")),
            segment_count=_as_int(data.get("segment_count")),
            total_tokens=_as_int(data.get("total_tokens")),
            resident_segments=_as_int(data.get("resident_segments")),
            indexed_segments=_as_int(data.get("indexed_segments")),
            saved_segments=_as_int(data.get("saved_segments")),
            indexed=_as_bool(data.get("indexed")),
            disk_cached=_as_bool(data.get("disk_cached")),
            plan_ready=_as_bool(data.get("plan_ready")),
            facts_dirty=_as_bool(data.get("facts_dirty")),
            kv_persist=_as_bool(data.get("kv_persist")),
        )


@dataclass
class SearchResult:
    text: str
    rank: int
    segment_id: int | None = None
    id: str | None = None
    memory_idx: int | None = None

    @classmethod
    def from_json(cls, data: Mapping[str, Any]) -> SearchResult:
        raw_id = data.get("id")
        text = data.get("text", "")
        return cls(
            id=raw_id if isinstance(raw_id, str) else None,
            memory_idx=_optional_int(data.get("memory_idx")),
            segment_id=_optional_int(data.get("segment_id")),
            text=text if isinstance(text, str) else "",
            rank=_as_int(data.get("rank")),
        )


def to_jsonable(value: Any) -> Any:
    if is_dataclass(value):
        return asdict(value)
    if isinstance(value, list):
        return [to_jsonable(item) for item in value]
    if isinstance(value, tuple):
        return [to_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): to_jsonable(item) for key, item in value.items()}
    return value
