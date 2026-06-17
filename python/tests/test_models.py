from __future__ import annotations

import pytest

from attemory.models import MemoryInput, SearchResult, SessionStatus, TokenUsage, to_jsonable


def test_memory_input_sends_text_and_optional_id() -> None:
    memory = MemoryInput.from_value({"id": "chunk-1", "text": "print('hello')"}, 0)

    assert memory.to_request_json() == {
        "id": "chunk-1",
        "text": "print('hello')",
    }


def test_memory_input_rejects_unsupported_fields() -> None:
    for field in ("code", "file", "lines"):
        with pytest.raises(TypeError, match=f"unsupported memory field: {field}"):
            MemoryInput.from_value({"text": "plain text", field: "ignored"}, 0)

    assert MemoryInput.from_value({"text": "plain text"}, 0).to_request_json() == {
        "text": "plain text"
    }


def test_memory_input_rejects_non_string_fields() -> None:
    with pytest.raises(TypeError, match="text must be a string"):
        MemoryInput.from_value({"text": 123}, 0)

    with pytest.raises(TypeError, match="id must be a string"):
        MemoryInput.from_value({"text": "ok", "id": 123}, 0)


def test_token_usage_parses_string_ints_and_optional_fields() -> None:
    usage = TokenUsage.from_json(
        {
            "prefill_tokens": "10",
            "ctx_length": 20,
            "remaining_tokens": "10",
            "segment_id": "1",
            "segment_count": 2,
            "memory_idx": "7",
            "id": "external-id",
        }
    )

    assert usage.prefill_tokens == 10
    assert usage.memory_idx == 7
    assert usage.id == "external-id"


def test_result_parsers_reject_bool_as_integer() -> None:
    with pytest.raises(TypeError, match="got bool"):
        TokenUsage.from_json(
            {
                "prefill_tokens": True,
                "ctx_length": 20,
                "remaining_tokens": 10,
                "segment_id": 1,
                "segment_count": 2,
            }
        )

    with pytest.raises(TypeError, match="got bool"):
        SearchResult.from_json({"rank": True, "text": "memory"})


def test_session_status_requires_boolean_fields() -> None:
    with pytest.raises(TypeError, match="expected boolean"):
        SessionStatus.from_json(
            {
                "session_id": "demo",
                "memory_count": 1,
                "segment_count": 1,
                "total_tokens": 10,
                "resident_segments": 1,
                "indexed_segments": 1,
                "saved_segments": 0,
                "indexed": "true",
                "disk_cached": False,
                "plan_ready": True,
                "facts_dirty": False,
            }
        )


def test_to_jsonable_recurses_through_dataclasses() -> None:
    result = SearchResult(rank=1, text="memory", id="m1", memory_idx=3, segment_id=2)

    assert to_jsonable({"results": [result]}) == {
        "results": [
            {
                "rank": 1,
                "text": "memory",
                "id": "m1",
                "memory_idx": 3,
                "segment_id": 2,
            }
        ]
    }
