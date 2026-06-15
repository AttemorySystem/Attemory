from __future__ import annotations

import os
import sys
from typing import NoReturn

from .model_resolver import ModelResolverError, prepare_server_args
from .runtime import resolve_runtime, runtime_environment


_NO_EQUALS_OPTIONS = {
    "-b",
    "-c",
    "-m",
    "-p",
    "--backend",
    "--cache-dir",
    "--chat-template",
    "--chat-template-file",
    "--config",
    "--data-dir",
    "--host",
    "--kv-type",
    "--kv-type-k",
    "--kv-type-v",
    "--model",
    "--model-info",
    "--n-batch",
    "--n-ctx",
    "--n-ubatch",
    "--port",
    "--resident-kv-budget",
    "--search-candidate-top-k",
    "--search-top-k",
    "--threads",
    "--threads-batch",
    "--v",
}


def main(argv: list[str] | None = None) -> int:
    server_args = list(sys.argv[1:] if argv is None else argv)

    try:
        runtime = resolve_runtime()
    except RuntimeError as exc:
        print(f"attemory-server: error: {exc}", file=sys.stderr)
        return 1

    validation_error = _validate_forwarded_args(server_args)
    if validation_error is not None:
        print(f"attemory-server: error: {validation_error}", file=sys.stderr)
        return 1

    try:
        server_args = prepare_server_args(server_args)
    except ModelResolverError as exc:
        print(f"attemory-server: error: {exc}", file=sys.stderr)
        return 1

    _exec_server(str(runtime.server_binary), server_args, runtime_environment(runtime))


def _validate_forwarded_args(args: list[str]) -> str | None:
    index = 0
    while index < len(args):
        arg = args[index]
        if "=" in arg:
            option = arg.split("=", 1)[0]
            if option in _NO_EQUALS_OPTIONS:
                return f"{option}=VALUE is not supported; use '{option} VALUE'"
        if arg in {"--port", "-p"}:
            if index + 1 >= len(args):
                return f"{arg} requires a value"
            value = args[index + 1]
            try:
                port = int(value, 10)
            except ValueError:
                return f"invalid port: {value}; expected an integer in 1..65535"
            if port < 1 or port > 65535:
                return f"invalid port: {value}; expected an integer in 1..65535"
            index += 2
            continue
        index += 1
    return None


def _exec_server(binary: str, args: list[str], env: dict[str, str]) -> NoReturn:
    argv = ["attemory-server", *args]
    try:
        os.execvpe(binary, argv, env)
    except OSError as exc:
        print(f"attemory-server: failed to execute {binary}: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    raise SystemExit(main())
