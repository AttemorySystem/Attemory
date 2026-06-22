from __future__ import annotations

import os
import socket
import subprocess
import time
from pathlib import Path

import pytest

from attemory.client import AttemoryClient
from attemory.exceptions import AttemoryError


SYSTEM_PROMPT = "Read the following memories carefully and find the most relevant memory to the query."


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _wait_for_server(
    client: AttemoryClient,
    process: subprocess.Popen[object],
    log_path: Path,
    *,
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            log = log_path.read_text(encoding="utf-8", errors="replace")
            if "model tiny file was not found" in log:
                pytest.skip("tiny model is not present in the Hugging Face cache")
            raise RuntimeError(f"attemory_server exited during startup:\n{log}")
        try:
            if client.health():
                return
        except (AttemoryError, OSError) as exc:
            last_error = exc
        time.sleep(0.1)

    log = log_path.read_text(encoding="utf-8", errors="replace")
    raise RuntimeError(f"attemory_server did not become healthy: {last_error}\n{log}")


def test_kv_persist_indexes_all_segments_with_tiny_server(tmp_path: Path) -> None:
    server_bin = os.environ.get("ATTEMORY_TEST_SERVER_BIN")
    if not server_bin:
        pytest.skip("set ATTEMORY_TEST_SERVER_BIN to run the tiny server kv-persist test")

    port = _free_port()
    log_path = tmp_path / "server.log"
    command = [
        server_bin,
        "--tiny",
        "--backend",
        "cpu",
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--data-dir",
        str(tmp_path / "sessions"),
        "--cache-dir",
        str(tmp_path / "cache"),
        "--n-ctx",
        "4096",
        "--resident-kv-budget",
        "1",
        "--search-top-k",
        "3",
        "--no-http-log",
        "--no-run-log",
    ]
    model_path = os.environ.get("ATTEMORY_TEST_MODEL")
    if model_path:
        command.extend(["--model", model_path])

    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)

    client = AttemoryClient(port=port, session_id="persist", timeout=3600.0)
    try:
        _wait_for_server(client, process, log_path, timeout=float(os.environ.get("ATTEMORY_TEST_TIMEOUT", "180")))

        client.create_session(kv_persist=True)
        client.add_system(SYSTEM_PROMPT)
        client.add_memory("The blue key is inside the ceramic bowl")
        client.next_segment()
        client.add_memory("The red key is under the desk")
        client.next_segment()
        client.add_memory("The green key is in the garage cabinet")
        client.index_session()

        status = next(status for status in client.list_sessions() if status.session_id == "persist")
        assert status.kv_persist is True
        assert status.segment_count == 3
        assert status.saved_segments == 3
        assert status.indexed_segments == 3
        assert status.indexed is True
        assert status.disk_cached is True
        assert status.resident_segments < status.segment_count

        client.clear_cache()
        assert client.search("Where is the blue key?", top_k=3)
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
