from __future__ import annotations

import argparse
import atexit
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

from .client import AttemoryClient
from .exceptions import AttemoryError, AttemoryHTTPError
from .models import MemoryInput, to_jsonable


DEFAULT_HEALTH_TIMEOUT_SECONDS = 30.0
DEFAULT_MCP_SYSTEM_PROMPT = (
    "Read the following memories carefully and find the most relevant memory "
    "to the query at the end."
)


class ManagedAttemoryServer:
    def __init__(
        self,
        *,
        binary: str,
        config: str | None,
        host: str,
        port: int,
        extra_args: list[str],
        log_file: str | None,
        timeout: float,
    ) -> None:
        self.binary = str(Path(binary).expanduser())
        self.config = str(Path(config).expanduser()) if config else None
        self.host = host
        self.port = port
        self.extra_args = extra_args
        self.log_file = str(Path(log_file).expanduser()) if log_file else None
        self.timeout = timeout
        self._process: subprocess.Popen[bytes] | None = None
        self._log_handle: Any | None = None

    def start(self) -> None:
        if self._process is not None:
            return

        command = [self.binary]
        if self.config:
            command.extend(["--config", self.config])
        command.extend(["--host", self.host, "--port", str(self.port)])
        command.extend(self.extra_args)

        stderr: Any = subprocess.DEVNULL
        if self.log_file:
            self._log_handle = open(self.log_file, "ab")
            stderr = self._log_handle

        self._process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
        atexit.register(self.stop)

        client = AttemoryClient(host=self.host, port=self.port, timeout=2.0)
        deadline = time.monotonic() + self.timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self._process.poll() is not None:
                raise RuntimeError(f"attemory server exited with code {self._process.returncode}")
            try:
                if client.health():
                    return
            except Exception as exc:  # noqa: BLE001 - health polling reports the last failure.
                last_error = exc
            time.sleep(0.25)

        self.stop()
        if last_error is not None:
            raise RuntimeError(f"attemory server did not become healthy: {last_error}") from last_error
        raise RuntimeError("attemory server did not become healthy")

    def stop(self) -> None:
        process = self._process
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5.0)
        self._process = None

        if self._log_handle is not None:
            self._log_handle.close()
            self._log_handle = None


class AttemoryMCPService:
    def __init__(
        self,
        *,
        client: AttemoryClient,
        system_prompt: str | None,
        auto_create_sessions: bool,
        auto_index: bool,
        auto_save: bool,
        search_waits_for_index: bool,
    ) -> None:
        self.client = client
        self.system_prompt = system_prompt
        self.auto_create_sessions = auto_create_sessions
        self.auto_index = auto_index
        self.auto_save = auto_save
        self.search_waits_for_index = search_waits_for_index
        self._lock = threading.RLock()

    def health(self) -> dict[str, Any]:
        with self._lock:
            if not self.client.health():
                raise RuntimeError("server health check failed")
            return {"status": "ok"}

    def list_sessions(self) -> dict[str, Any]:
        with self._lock:
            return {"sessions": to_jsonable(self.client.list_sessions())}

    def session_status(self, session_id: str) -> dict[str, Any]:
        with self._lock:
            self._ensure_session_loaded(session_id)
            return self._status_response(session_id)

    def create_session(self, session_id: str) -> dict[str, Any]:
        with self._lock:
            usage = self.client.create_session(session_id=session_id)
            system_usage = None
            if self.system_prompt:
                system_usage = self.client.add_system(self.system_prompt, session_id=session_id)
            return {
                "session_id": session_id,
                "usage": to_jsonable(usage),
                "system_usage": to_jsonable(system_usage) if system_usage is not None else None,
                "status": self._status_response(session_id).get("status"),
            }

    def add_memory(self, session_id: str, text: str, id: str | None = None) -> dict[str, Any]:
        with self._lock:
            self._ensure_session_loaded(session_id)
            usage = self.client.add_memory(text, session_id=session_id, id=id)

            response: dict[str, Any] = {
                "session_id": session_id,
                "usage": to_jsonable(usage),
                "index": None,
                "save": None,
            }
            if self.auto_index:
                response["index"] = self.client.index_session(session_id=session_id)
            if self.auto_save:
                response["save"] = self.client.save_session(session_id=session_id)
            response["status"] = self._status_response(session_id).get("status")
            return response

    def save_session(self, session_id: str) -> dict[str, Any]:
        with self._lock:
            self._ensure_session_loaded(session_id)
            response: dict[str, Any] = {"session_id": session_id, "index": None}
            if self.auto_index:
                response["index"] = self.client.index_session(session_id=session_id)
            response["save"] = self.client.save_session(session_id=session_id)
            response["status"] = self._status_response(session_id).get("status")
            return response

    def search(
        self,
        session_id: str,
        query: str,
        *,
        query_context: str | None,
        top_k: int | None,
    ) -> dict[str, Any]:
        with self._lock:
            self._ensure_session_loaded(session_id)
            index_result = None
            if self.auto_index and self.search_waits_for_index:
                status = self._status(session_id)
                if status is not None and not status.indexed:
                    index_result = self.client.index_session(session_id=session_id)
                    if self.auto_save:
                        self.client.save_session(session_id=session_id)
            results = self.client.search(
                query,
                session_id=session_id,
                query_context=query_context,
                top_k=top_k,
            )
            return {
                "session_id": session_id,
                "index": index_result,
                "results": to_jsonable(results),
            }

    def oneshot_search(
        self,
        query: str,
        memories: list[dict[str, Any]],
        *,
        query_context: str | None,
        top_k: int | None,
    ) -> dict[str, Any]:
        normalized = [MemoryInput.from_value(memory, index) for index, memory in enumerate(memories)]
        with self._lock:
            results = self.client.oneshot_search(
                query,
                normalized,
                system=self.system_prompt or DEFAULT_MCP_SYSTEM_PROMPT,
                query_context=query_context,
                top_k=top_k,
            )
        return {"results": to_jsonable(results)}

    def _ensure_session_loaded(self, session_id: str) -> None:
        try:
            self.client.restore_session(session_id=session_id)
            return
        except AttemoryError:
            if not self.auto_create_sessions:
                raise

        self.client.create_session(session_id=session_id)
        if self.system_prompt:
            self.client.add_system(self.system_prompt, session_id=session_id)

    def _status_response(self, session_id: str) -> dict[str, Any]:
        status = self._status(session_id)
        if status is None:
            raise RuntimeError("session is not loaded")
        return {"session_id": session_id, "status": to_jsonable(status)}

    def _status(self, session_id: str) -> Any | None:
        for status in self.client.list_sessions():
            if status.session_id == session_id:
                return status
        return None


def build_mcp_app(service: AttemoryMCPService) -> Any:
    try:
        from mcp.server.fastmcp import FastMCP
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "MCP support requires the optional dependency: "
            "pip install 'attemory[mcp]' or pip install mcp"
        ) from exc

    mcp = FastMCP(
        "attemory",
        instructions=(
            "Use attemory for long-term memory search. Tools intentionally expose "
            "semantic session and search operations only; restore, system prompts, "
            "indexing, and saving are handled internally by this MCP server."
        ),
    )

    @mcp.tool()
    def attemory_health() -> dict[str, Any]:
        """Check whether the underlying attemory HTTP server is reachable."""
        return _run_tool(service.health)

    @mcp.tool()
    def attemory_list_sessions() -> dict[str, Any]:
        """List sessions currently loaded by the attemory server."""
        return _run_tool(service.list_sessions)

    @mcp.tool()
    def attemory_session_status(session_id: str) -> dict[str, Any]:
        """Return visible status for a session; loads the session internally if needed."""
        return _run_tool(service.session_status, session_id)

    @mcp.tool()
    def attemory_create_session(session_id: str) -> dict[str, Any]:
        """Create a memory session. The MCP server applies its configured system prompt internally."""
        return _run_tool(service.create_session, session_id)

    @mcp.tool()
    def attemory_add_memory(session_id: str, text: str, id: str | None = None) -> dict[str, Any]:
        """Add memory text to a session with an optional opaque client id."""
        return _run_tool(service.add_memory, session_id, text, id)

    @mcp.tool()
    def attemory_search(
        session_id: str,
        query: str,
        query_context: str | None = None,
        top_k: int | None = 20,
    ) -> dict[str, Any]:
        """Search a session by id. query_context is prefetched but excluded from direct attention scoring."""
        return _run_tool(
            service.search,
            session_id,
            query,
            query_context=query_context,
            top_k=top_k,
        )

    @mcp.tool()
    def attemory_oneshot_search(
        query: str,
        memories: list[dict[str, Any]],
        query_context: str | None = None,
        top_k: int | None = 20,
    ) -> dict[str, Any]:
        """Run one-shot search. query_context is prefetched but excluded from direct attention scoring."""
        return _run_tool(
            service.oneshot_search,
            query,
            memories,
            query_context=query_context,
            top_k=top_k,
        )

    @mcp.tool()
    def attemory_save_session(session_id: str) -> dict[str, Any]:
        """Persist a session according to the MCP server policy."""
        return _run_tool(service.save_session, session_id)

    @mcp.resource("attemory://sessions")
    def attemory_sessions_resource() -> str:
        """Loaded attemory sessions."""
        return json.dumps(_run_tool(service.list_sessions), ensure_ascii=False)

    @mcp.resource("attemory://sessions/{session_id}/status")
    def attemory_session_status_resource(session_id: str) -> str:
        """Status for one attemory session."""
        return json.dumps(_run_tool(service.session_status, session_id), ensure_ascii=False)

    return mcp


def _run_tool(func: Any, *args: Any, **kwargs: Any) -> dict[str, Any]:
    try:
        return {"data": func(*args, **kwargs)}
    except Exception as exc:  # noqa: BLE001 - MCP tools should report structured failures.
        return {"error": _error_payload(exc)}


def _error_payload(exc: BaseException) -> dict[str, Any]:
    code = "ATTEMORY_ERROR"
    message = str(exc)
    details: dict[str, Any] = {"error_type": type(exc).__name__}

    if isinstance(exc, AttemoryHTTPError):
        if exc.error_code:
            code = exc.error_code
        if exc.error_message:
            message = exc.error_message
        details["status_code"] = exc.status_code
        if exc.details is not None:
            details["server"] = exc.details
    elif isinstance(exc, (ValueError, TypeError)):
        code = "INVALID_REQUEST"
    elif isinstance(exc, RuntimeError):
        code = "RUNTIME_ERROR"

    return {
        "code": code,
        "message": message,
        "details": details,
    }


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    manager: ManagedAttemoryServer | None = None
    if args.manage_server:
        manager = ManagedAttemoryServer(
            binary=args.attemory_bin,
            config=args.config,
            host=args.host,
            port=args.port,
            extra_args=args.server_arg,
            log_file=args.server_log,
            timeout=args.health_timeout,
        )
        try:
            manager.start()
        except Exception as exc:  # noqa: BLE001 - startup failures must be surfaced before MCP starts.
            print(f"attemory-mcp: error: {exc}", file=sys.stderr)
            return 1

    client = AttemoryClient(
        host=args.host,
        port=args.port,
        timeout=args.timeout,
    )
    service = AttemoryMCPService(
        client=client,
        system_prompt=_read_system_prompt(args),
        auto_create_sessions=args.auto_create_sessions,
        auto_index=not args.no_auto_index,
        auto_save=not args.no_auto_save,
        search_waits_for_index=not args.no_search_waits_for_index,
    )

    try:
        app = build_mcp_app(service)
        app.run(transport=args.transport)
    except Exception as exc:  # noqa: BLE001
        print(f"attemory-mcp: error: {exc}", file=sys.stderr)
        return 1
    finally:
        if manager is not None:
            manager.stop()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="attemory-mcp", description="MCP adapter for attemory")
    parser.add_argument("--host", default=os.environ.get("ATTEMORY_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("ATTEMORY_PORT", "9006")))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("ATTEMORY_TIMEOUT", "3600")))
    parser.add_argument(
        "--transport",
        choices=["stdio", "streamable-http"],
        default=os.environ.get("ATTEMORY_MCP_TRANSPORT", "stdio"),
    )
    parser.add_argument("--system-prompt", default=os.environ.get("ATTEMORY_MCP_SYSTEM_PROMPT"))
    parser.add_argument("--system-prompt-file", default=os.environ.get("ATTEMORY_MCP_SYSTEM_PROMPT_FILE"))
    parser.add_argument("--no-system-prompt", action="store_true")
    parser.add_argument("--auto-create-sessions", action="store_true")
    parser.add_argument("--no-auto-index", action="store_true")
    parser.add_argument("--no-auto-save", action="store_true")
    parser.add_argument("--no-search-waits-for-index", action="store_true")

    parser.add_argument("--manage-server", action="store_true")
    parser.add_argument("--attemory-bin", default=os.environ.get("ATTEMORY_BIN", "attemory-server"))
    parser.add_argument("--config", default=os.environ.get("ATTEMORY_SERVER_CONFIG"))
    parser.add_argument("--server-log", default=os.environ.get("ATTEMORY_SERVER_LOG"))
    parser.add_argument("--server-arg", action="append", default=[])
    parser.add_argument("--health-timeout", type=float, default=DEFAULT_HEALTH_TIMEOUT_SECONDS)
    return parser


def _read_system_prompt(args: argparse.Namespace) -> str | None:
    if args.no_system_prompt:
        return None
    if args.system_prompt_file:
        return Path(args.system_prompt_file).expanduser().read_text(encoding="utf-8")
    if args.system_prompt is not None:
        return args.system_prompt
    return DEFAULT_MCP_SYSTEM_PROMPT


if __name__ == "__main__":
    raise SystemExit(main())
