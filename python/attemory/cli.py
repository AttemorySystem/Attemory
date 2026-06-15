from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

from .client import AttemoryClient, DEFAULT_SYSTEM_PROMPT
from .exceptions import AttemoryError, AttemoryHTTPError
from .models import MemoryInput, to_jsonable


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    client = AttemoryClient(
        host=args.host,
        port=args.port,
        session_id=args.session,
        timeout=args.timeout,
    )

    try:
        output = args.handler(client, args)
    except (AttemoryError, OSError, ValueError, TypeError) as exc:
        print_json(error_envelope(exc), compact=args.compact_json, file=sys.stderr)
        return 1

    if output is not None:
        print_json(data_envelope(output), compact=args.compact_json)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="attemory", description="CLI client for attemory server")
    parser.add_argument("--host", default=os.environ.get("ATTEMORY_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("ATTEMORY_PORT", "9006")))
    parser.add_argument(
        "--timeout",
        type=float,
        default=float(os.environ.get("ATTEMORY_TIMEOUT", "3600")),
    )
    parser.add_argument("-s", "--session", default=os.environ.get("ATTEMORY_SESSION"))
    parser.add_argument("--compact-json", action="store_true", help="print compact JSON")

    subparsers = parser.add_subparsers(dest="command", required=True)

    _add_simple_command(subparsers, "health", run_health)
    _add_simple_command(subparsers, "sessions", run_sessions)
    _add_session_command(subparsers, "create", run_create)
    _add_session_command(subparsers, "delete", run_delete)
    _add_session_command(subparsers, "restore", run_restore)
    _add_session_command(subparsers, "clear-cache", run_clear_cache)
    _add_session_command(subparsers, "index", run_index)
    _add_session_command(subparsers, "save", run_save)
    _add_session_command(subparsers, "next-segment", run_next_segment)

    add_system = _add_session_command(subparsers, "add-system", run_add_system)
    _add_text_options(add_system)

    add_memory = _add_session_command(subparsers, "add-memory", run_add_memory)
    _add_text_options(add_memory)
    add_memory.add_argument("--id", default=None, help="optional client memory id")

    search = _add_session_command(subparsers, "search", run_search)
    _add_text_options(search)
    _add_query_context_options(search)
    _add_search_options(search)

    oneshot = subparsers.add_parser("oneshot-search", help="run one-shot search")
    oneshot.set_defaults(handler=run_oneshot_search)
    _add_query_options(oneshot)
    _add_query_context_options(oneshot)
    _add_system_options(oneshot)
    _add_search_options(oneshot)
    oneshot.add_argument("--memory", action="append", default=[], help="memory text; repeatable")
    oneshot.add_argument(
        "--memory-file",
        action="append",
        default=[],
        help="file whose content is one memory; repeatable",
    )
    oneshot.add_argument(
        "--memories-json",
        help="JSON file containing a list of memories or an object with memories",
    )
    return parser


def _add_simple_command(
    subparsers: Any,
    name: str,
    handler: Any,
) -> argparse.ArgumentParser:
    command = subparsers.add_parser(name)
    command.set_defaults(handler=handler)
    return command


def _add_session_command(
    subparsers: Any,
    name: str,
    handler: Any,
) -> argparse.ArgumentParser:
    command = subparsers.add_parser(name)
    command.add_argument(
        "session_id",
        nargs="?",
        help="defaults to global --session or ATTEMORY_SESSION",
    )
    command.set_defaults(handler=handler)
    return command


def _add_text_options(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--text", help="inline UTF-8 text")
    group.add_argument("--file", help="read UTF-8 text from file")


def _add_query_options(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--query", help="inline query text")
    group.add_argument("--query-file", help="read query text from file")


def _add_query_context_options(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--query-context", help="inline context prefetched before query scoring")
    group.add_argument("--query-context-file", help="read query context from file")


def _add_system_options(parser: argparse.ArgumentParser) -> None:
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--system", default=None, help="inline system prompt")
    group.add_argument("--system-file", help="read system prompt from file")


def _add_search_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--top-k", type=int, default=None)


def run_health(client: AttemoryClient, _: argparse.Namespace) -> dict[str, str]:
    if not client.health():
        raise AttemoryError("server health check failed")
    return {"status": "ok"}


def run_sessions(client: AttemoryClient, _: argparse.Namespace) -> list[Any]:
    return client.list_sessions()


def run_create(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.create_session(_session_id(client, args))


def run_delete(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.delete_session(_session_id(client, args))


def run_restore(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.restore_session(_session_id(client, args))


def run_clear_cache(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.clear_cache(_session_id(client, args))


def run_index(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.index_session(_session_id(client, args))


def run_save(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.save_session(_session_id(client, args))


def run_next_segment(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.next_segment(_session_id(client, args))


def run_add_system(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.add_system(_read_text(args), _session_id(client, args))


def run_add_memory(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.add_memory(_read_text(args), _session_id(client, args), id=args.id)


def run_search(client: AttemoryClient, args: argparse.Namespace) -> Any:
    return client.search(
        _read_text(args),
        session_id=_session_id(client, args),
        query_context=_read_query_context(args),
        top_k=args.top_k,
    )


def run_oneshot_search(client: AttemoryClient, args: argparse.Namespace) -> Any:
    memories = _read_memories(args)
    if not memories:
        raise ValueError("oneshot-search requires at least one memory")
    return client.oneshot_search(
        _read_query(args),
        memories,
        system=_read_system(args),
        query_context=_read_query_context(args),
        top_k=args.top_k,
    )


def _session_id(client: AttemoryClient, args: argparse.Namespace) -> str:
    session_id = getattr(args, "session_id", None) or client.session_id
    if not session_id:
        raise ValueError("session id is required; pass positional session_id or global --session")
    return session_id


def _read_text(args: argparse.Namespace) -> str:
    if getattr(args, "text", None) is not None:
        return args.text
    if getattr(args, "file", None):
        return Path(args.file).read_text(encoding="utf-8")
    return sys.stdin.read()


def _read_query(args: argparse.Namespace) -> str:
    if args.query is not None:
        return args.query
    return Path(args.query_file).read_text(encoding="utf-8")


def _read_query_context(args: argparse.Namespace) -> str | None:
    if getattr(args, "query_context", None) is not None:
        return args.query_context
    if getattr(args, "query_context_file", None):
        return Path(args.query_context_file).read_text(encoding="utf-8")
    return None


def _read_system(args: argparse.Namespace) -> str:
    if args.system is not None:
        return args.system
    if args.system_file:
        return Path(args.system_file).read_text(encoding="utf-8")
    return DEFAULT_SYSTEM_PROMPT


def _read_memories(args: argparse.Namespace) -> list[MemoryInput]:
    memories: list[MemoryInput] = []
    for text in args.memory:
        memories.append(MemoryInput(text=text))
    for filename in args.memory_file:
        path = Path(filename)
        memories.append(
            MemoryInput(
                text=path.read_text(encoding="utf-8"),
                file=path.as_posix(),
            )
        )
    if args.memories_json:
        loaded = json.loads(Path(args.memories_json).read_text(encoding="utf-8"))
        if isinstance(loaded, dict):
            loaded = loaded.get("memories", [])
        if not isinstance(loaded, list):
            raise ValueError("memories JSON must be a list or an object with memories")
        base_index = len(memories)
        memories.extend(
            MemoryInput.from_value(item, base_index + index)
            for index, item in enumerate(loaded)
        )
    return memories


def data_envelope(data: Any) -> dict[str, Any]:
    return {"data": data}


def error_envelope(exc: BaseException) -> dict[str, Any]:
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
    elif isinstance(exc, OSError):
        code = "IO_ERROR"

    return {
        "error": {
            "code": code,
            "message": message,
            "details": details,
        }
    }


def print_json(value: Any, *, compact: bool, file: Any = None) -> None:
    if compact:
        print(json.dumps(to_jsonable(value), ensure_ascii=False, separators=(",", ":")), file=file)
    else:
        print(json.dumps(to_jsonable(value), ensure_ascii=False, indent=2), file=file)


if __name__ == "__main__":
    raise SystemExit(main())
