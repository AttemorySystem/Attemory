#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import random
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path


PYTHON_ROOT = Path(__file__).resolve().parents[1]
if str(PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYTHON_ROOT))

from attemory import AttemoryClient, MemoryInput  # noqa: E402
from attemory.exceptions import AttemoryHTTPError  # noqa: E402
from attemory.models import SearchResult, SessionStatus  # noqa: E402


SYSTEM_PROMPT = (
    "Read the following memories carefully and find the most relevant memory "
    "to the query at the end."
)


@dataclass
class SessionProbe:
    session_id: str
    memory_ids: list[str] = field(default_factory=list)
    keywords: list[str] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Black-box attemory_server stability test. Start attemory_server "
            "separately before running this script."
        )
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9006)
    parser.add_argument("--sessions", type=int, default=4)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--top-k", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=3600.0)
    parser.add_argument("--seed", type=int, default=20260611)
    parser.add_argument(
        "--session-prefix",
        default="stability",
        help="Prefix for generated session ids.",
    )
    parser.add_argument(
        "--keep-sessions",
        action="store_true",
        help="Do not delete generated sessions at the end.",
    )
    parser.add_argument(
        "--skip-clear-restore",
        action="store_true",
        help="Skip clear-cache + restore cycles after save.",
    )
    return parser.parse_args()


def log(message: str) -> None:
    print(message, flush=True)


def fail(message: str) -> None:
    raise RuntimeError(message)


def expect(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def unique_run_prefix(prefix: str) -> str:
    return f"{prefix}-{int(time.time())}-{os.getpid()}"


def create_probe_sessions(client: AttemoryClient, run_prefix: str, count: int) -> list[SessionProbe]:
    probes: list[SessionProbe] = []
    for index in range(count):
        session_id = f"{run_prefix}-s{index}"
        session = client.with_session(session_id)
        log(f"[create] {session_id}")
        session.create_session()
        session.add_system(SYSTEM_PROMPT)
        probes.append(SessionProbe(session_id=session_id))
    return probes


def memory_text(session_id: str, round_index: int, slot: str, keyword: str) -> str:
    return (
        f"Session {session_id} round {round_index} memory {slot}. "
        f"The stability keyword is {keyword}. "
        f"Keep this fact isolated from other sessions."
    )


def add_probe_memory(
    session: AttemoryClient,
    probe: SessionProbe,
    round_index: int,
    slot: str,
) -> str:
    keyword = f"kw-{probe.session_id}-r{round_index}-{slot}"
    memory_id = f"{probe.session_id}-mem-r{round_index}-{slot}"
    session.add_memory(
        MemoryInput(id=memory_id, text=memory_text(probe.session_id, round_index, slot, keyword))
    )
    probe.memory_ids.append(memory_id)
    probe.keywords.append(keyword)
    log(f"[add-memory] {probe.session_id} {memory_id}")
    return keyword


def require_session_status(
    statuses: list[SessionStatus],
    session_id: str,
) -> SessionStatus:
    for status in statuses:
        if status.session_id == session_id:
            return status
    fail(f"missing session status for {session_id}")


def assert_status_consistent(status: SessionStatus, probe: SessionProbe) -> None:
    expect(status.memory_count == len(probe.memory_ids), f"{status.session_id}: memory_count mismatch")
    if not probe.memory_ids:
        expect(status.segment_count == 0, f"{status.session_id}: empty session should not have segments")
        return

    expect(status.segment_count >= 1, f"{status.session_id}: expected at least one segment")
    expect(status.indexed, f"{status.session_id}: expected indexed=true")
    expect(
        status.indexed_segments == status.segment_count,
        f"{status.session_id}: indexed segment count mismatch",
    )
    expect(status.total_tokens > 0, f"{status.session_id}: expected positive total_tokens")


def assert_search_result(
    session: AttemoryClient,
    probe: SessionProbe,
    keyword: str,
    *,
    top_k: int,
) -> None:
    results = session.search(
        f"Which memory contains stability keyword {keyword}?",
        query_context=f"Current stability test session is {probe.session_id}.",
        top_k=top_k,
    )
    expect(results, f"{probe.session_id}: search returned no results")
    assert_result_belongs_to_session(results, probe)


def assert_result_belongs_to_session(results: list[SearchResult], probe: SessionProbe) -> None:
    known_ids = set(probe.memory_ids)
    for result in results:
        if result.id is None:
            continue
        expect(
            result.id in known_ids,
            f"{probe.session_id}: search leaked result id from another session: {result.id}",
        )


def save_restore_cycle(session: AttemoryClient, probe: SessionProbe, *, clear_restore: bool) -> None:
    log(f"[save] {probe.session_id}")
    session.save_session()
    if not clear_restore:
        return

    log(f"[clear-cache] {probe.session_id}")
    session.clear_cache()
    log(f"[restore] {probe.session_id}")
    session.restore_session()


def exercise_session(
    client: AttemoryClient,
    probe: SessionProbe,
    round_index: int,
    *,
    top_k: int,
    clear_restore: bool,
) -> None:
    session = client.with_session(probe.session_id)
    log(f"[round {round_index}] {probe.session_id}")

    first_keyword = add_probe_memory(session, probe, round_index, "a")
    session.index_session()
    assert_search_result(session, probe, first_keyword, top_k=top_k)

    second_keyword = add_probe_memory(session, probe, round_index, "b")
    session.index_session()
    save_restore_cycle(session, probe, clear_restore=clear_restore)
    assert_search_result(session, probe, second_keyword, top_k=top_k)


def verify_all_statuses(client: AttemoryClient, probes: list[SessionProbe]) -> None:
    statuses = client.list_sessions()
    for probe in probes:
        status = require_session_status(statuses, probe.session_id)
        assert_status_consistent(status, probe)


def cleanup_sessions(client: AttemoryClient, probes: list[SessionProbe]) -> None:
    for probe in probes:
        try:
            log(f"[delete] {probe.session_id}")
            client.delete_session(probe.session_id)
        except AttemoryHTTPError as exc:
            if exc.status_code != 404:
                raise


def run() -> None:
    args = parse_args()
    rng = random.Random(args.seed)
    client = AttemoryClient(host=args.host, port=args.port, timeout=args.timeout)

    expect(client.health(), "server health check failed")
    run_prefix = unique_run_prefix(args.session_prefix)
    probes = create_probe_sessions(client, run_prefix, args.sessions)

    try:
        for round_index in range(args.rounds):
            order = list(probes)
            rng.shuffle(order)
            for probe in order:
                exercise_session(
                    client,
                    probe,
                    round_index,
                    top_k=args.top_k,
                    clear_restore=not args.skip_clear_restore,
                )
                verify_all_statuses(client, probes)

        for probe in reversed(probes):
            session = client.with_session(probe.session_id)
            session.restore_session()
            session.index_session()
            if probe.keywords:
                assert_search_result(session, probe, probe.keywords[-1], top_k=args.top_k)

        verify_all_statuses(client, probes)
        log("[done] server stability test completed")
    finally:
        if not args.keep_sessions:
            cleanup_sessions(client, probes)


if __name__ == "__main__":
    run()
