from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import quote

from .runtime import RuntimeInfo, resolve_runtime, runtime_environment


class ModelResolverError(RuntimeError):
    pass


_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
_CACHE_KEY_RE = re.compile(r"^[A-Za-z0-9._-]+$")
_DOWNLOAD_CHUNK_BYTES = 8 * 1024 * 1024
_DOWNLOAD_LOG_BYTES = 512 * 1024 * 1024


def _model_info(tier: str) -> dict[str, Any]:
    runtime = _resolve_runtime()
    data = _run_metadata_command(runtime, ["--model-info", tier])
    if not isinstance(data, dict):
        raise ModelResolverError("native model tier response is not a JSON object")
    return data


def _resolve_or_fetch_model(
    tier: str,
    *,
    force: bool = False,
) -> Path:
    info = _model_info(tier)
    existing = find_model_file(info)
    if existing is not None and not force:
        return existing
    return _fetch_model(info, force=force)


def find_model_file(info: dict[str, Any]) -> Path | None:
    filename = _required_string(info, "filename")
    revision = _optional_string(info, "revision") or "main"

    repo = _optional_string(info, "hf_repo")
    if not repo:
        return None
    return _find_hf_cache_file(repo, revision, filename)


def _fetch_model(info: dict[str, Any], *, force: bool = False) -> Path:
    tier = _required_string(info, "tier")
    repo = _required_string(info, "hf_repo")
    filename = _required_string(info, "filename")
    revision = _optional_string(info, "revision") or "main"
    sha256 = _optional_string(info, "sha256")

    if _hf_offline():
        cached = find_model_file(info)
        if cached is not None:
            return cached
        raise ModelResolverError(
            f"model {tier} is not cached and HF_HUB_OFFLINE is enabled; "
            "unset HF_HUB_OFFLINE or pass --model to a local GGUF file"
        )

    requests = _require_requests()
    url = _resolve_url(repo, revision, filename)
    try:
        print(
            f"Starting download of {tier} model from {repo}/{filename}",
            file=sys.stderr,
            flush=True,
        )
        with requests.Session() as session:
            session.trust_env = True
            response_context = session.get(
                url,
                headers=_request_headers(),
                stream=True,
                timeout=(30, 300),
                allow_redirects=True,
            )
            with response_context as response:
                if response.status_code >= 400:
                    raise ModelResolverError(_http_error(url, response))

                commit = _clean_commit(response.headers.get("x-repo-commit")) or revision
                etag = _clean_cache_key(response.headers.get("etag"))
                target = _download_target(repo, revision, filename, commit, etag)

                if target.exists() and not force:
                    return target

                blob = _blob_path(repo, etag) if etag else None
                if blob is not None and blob.exists() and not force:
                    _ensure_snapshot_entry(target, blob)
                    _write_ref(repo, revision, commit)
                    return target

                print(
                    f"Downloading {tier} model from {repo}/{filename} to {target}",
                    file=sys.stderr,
                    flush=True,
                )
                download_path = blob if blob is not None else target
                _download_response(response, download_path, sha256, tier)
                if blob is not None:
                    _ensure_snapshot_entry(target, blob)
                _write_ref(repo, revision, commit)
                return target
    except ModelResolverError:
        raise
    except Exception as exc:
        raise ModelResolverError(f"failed to download {tier} from {repo}/{filename}: {exc}") from exc


def prepare_server_args(
    args: list[str],
) -> list[str]:
    if _has_any_arg(args, {"--model-info", "--version", "--help", "-h"}):
        return args

    tier, model_path = _final_model_selection(args)
    if model_path or not tier:
        return args

    path = _resolve_or_fetch_model(tier)
    return [*args, "--model", str(path)]


def _final_model_selection(args: list[str]) -> tuple[str | None, str | None]:
    tier: str | None = None
    model_path: str | None = None

    config_path = _arg_value(args, "--config")
    if config_path:
        tier, model_path = _read_config_model(Path(config_path).expanduser())

    index = 0
    while index < len(args):
        arg = args[index]
        arg_tier = _tier_from_arg(arg)
        if arg_tier is not None:
            tier = arg_tier
            model_path = None
            index += 1
            continue
        if arg in {"--model", "-m"} and index + 1 < len(args):
            model_path = args[index + 1]
            index += 2
            continue
        index += 1

    return tier, model_path


def _tier_from_arg(arg: str) -> str | None:
    if arg == "--tiny":
        return "tiny"
    if arg == "--small":
        return "small"
    if arg == "--medium":
        return "medium"
    if arg == "--large":
        return "large"
    return None


def _resolve_runtime() -> RuntimeInfo:
    try:
        return resolve_runtime()
    except RuntimeError as exc:
        raise ModelResolverError(str(exc)) from exc


def _run_metadata_command(runtime: RuntimeInfo, args: list[str]) -> Any:
    command = [str(runtime.server_binary), *args]
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=runtime_environment(runtime),
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ModelResolverError(f"{' '.join(command)} failed: {detail}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise ModelResolverError("native model metadata response is not valid JSON") from exc


def _read_config_model(path: Path) -> tuple[str | None, str | None]:
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ModelResolverError(f"failed to read config file {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ModelResolverError(f"config file is not valid JSON: {path}") from exc

    model = config.get("model") if isinstance(config, dict) else None
    if isinstance(model, str):
        return model, None
    if isinstance(model, dict):
        tier = model.get("tier")
        model_path = model.get("path")
        return (
            tier if isinstance(tier, str) and tier else None,
            model_path if isinstance(model_path, str) and model_path else None,
        )
    return None, None


def _arg_value(args: list[str], name: str) -> str | None:
    for index, arg in enumerate(args):
        if arg == name and index + 1 < len(args):
            return args[index + 1]
    return None


def _has_any_arg(args: list[str], names: set[str]) -> bool:
    return any(arg in names for arg in args)


def _required_string(info: dict[str, Any], key: str) -> str:
    value = info.get(key)
    if not isinstance(value, str) or not value:
        raise ModelResolverError(f"model metadata is missing {key}")
    return value


def _optional_string(info: dict[str, Any], key: str) -> str | None:
    value = info.get(key)
    return value if isinstance(value, str) and value else None


def _require_requests() -> Any:
    try:
        import requests
    except ModuleNotFoundError as exc:
        raise ModelResolverError(
            "requests is required for model downloads but is not installed; "
            "reinstall the attemory Python package"
        ) from exc
    return requests


def _hf_token() -> str | None:
    return os.environ.get("HF_TOKEN") or os.environ.get("HUGGING_FACE_HUB_TOKEN")


def _hf_offline() -> bool:
    value = os.environ.get("HF_HUB_OFFLINE", "")
    return value == "1" or value.lower() == "true"


def _hf_endpoint() -> str:
    endpoint = (
        os.environ.get("HF_ENDPOINT")
        or os.environ.get("MODEL_ENDPOINT")
        or "https://huggingface.co"
    )
    return endpoint.rstrip("/")


def _request_headers() -> dict[str, str]:
    headers = {"User-Agent": "attemory/0.1"}
    token = _hf_token()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    return headers


def _resolve_url(repo: str, revision: str, filename: str) -> str:
    return (
        f"{_hf_endpoint()}/"
        f"{quote(repo, safe='/')}/resolve/{quote(revision, safe='')}/"
        f"{quote(filename, safe='/')}"
    )


def _http_error(url: str, response: Any) -> str:
    detail = response.text[:512].strip() if getattr(response, "text", None) else ""
    message = f"HTTP {response.status_code} while downloading {url}"
    if detail:
        message += f": {detail}"
    return message


def _hf_cache_dir() -> Path:
    hf_hub_cache = _expand_env_path("HF_HUB_CACHE")
    if hf_hub_cache is not None:
        return hf_hub_cache
    huggingface_hub_cache = _expand_env_path("HUGGINGFACE_HUB_CACHE")
    if huggingface_hub_cache is not None:
        return huggingface_hub_cache
    hf_home = _expand_env_path("HF_HOME")
    if hf_home is not None:
        return hf_home / "hub"
    xdg_cache = _expand_env_path("XDG_CACHE_HOME")
    if xdg_cache is not None:
        return xdg_cache / "huggingface" / "hub"
    return Path.home() / ".cache" / "huggingface" / "hub"


def _expand_env_path(name: str) -> Path | None:
    value = os.environ.get(name)
    if not value:
        return None
    return Path(value).expanduser()


def _repo_cache_name(repo: str) -> str:
    return "models--" + repo.replace("/", "--")


def _repo_cache_dir(repo: str) -> Path:
    return _hf_cache_dir() / _repo_cache_name(repo)


def _relative_filename(filename: str) -> Path:
    path = PurePosixPath(filename)
    if path.is_absolute() or ".." in path.parts:
        raise ModelResolverError(f"unsafe model filename from registry: {filename}")
    return Path(*path.parts)


def _snapshot_path(repo: str, snapshot: str, filename: str) -> Path:
    return _repo_cache_dir(repo) / "snapshots" / snapshot / _relative_filename(filename)


def _blob_path(repo: str, key: str | None) -> Path | None:
    if not key:
        return None
    return _repo_cache_dir(repo) / "blobs" / key


def _find_hf_cache_file(repo: str, revision: str, filename: str) -> Path | None:
    repo_dir = _repo_cache_dir(repo)
    candidates: list[str] = []

    ref = repo_dir / "refs" / revision
    try:
        resolved = ref.read_text(encoding="utf-8").strip()
    except OSError:
        resolved = ""
    if resolved:
        candidates.append(resolved)
    candidates.append(revision)

    for snapshot in candidates:
        candidate = _snapshot_path(repo, snapshot, filename)
        if candidate.exists():
            return candidate.expanduser()

    snapshots = repo_dir / "snapshots"
    try:
        entries = sorted(snapshots.iterdir(), key=lambda item: item.stat().st_mtime, reverse=True)
    except OSError:
        return None
    for entry in entries:
        candidate = entry / _relative_filename(filename)
        if candidate.exists():
            return candidate.expanduser()
    return None


def _download_target(
    repo: str,
    revision: str,
    filename: str,
    commit: str,
    etag: str | None,
) -> Path:
    snapshot = commit if _COMMIT_RE.match(commit) else revision
    return _snapshot_path(repo, snapshot, filename)


def _clean_commit(value: str | None) -> str | None:
    if not value:
        return None
    value = value.strip()
    return value if _COMMIT_RE.match(value) else None


def _clean_cache_key(value: str | None) -> str | None:
    if not value:
        return None
    value = value.strip()
    if value.startswith("W/"):
        value = value[2:].strip()
    value = value.strip('"')
    if value in {".", ".."}:
        return None
    return value if _CACHE_KEY_RE.match(value) else None


def _download_response(response: Any, target: Path, sha256: str | None, tier: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + ".download")
    digest = hashlib.sha256() if sha256 else None
    total = _response_content_length(response)
    downloaded = 0
    last_percent = 0 if total is not None else -1
    last_byte_report = 0
    _print_download_progress(tier, downloaded, total, final=False)
    try:
        with temp.open("wb") as out:
            for chunk in response.iter_content(chunk_size=_DOWNLOAD_CHUNK_BYTES):
                if not chunk:
                    continue
                out.write(chunk)
                downloaded += len(chunk)
                if digest is not None:
                    digest.update(chunk)
                if total is not None:
                    percent = min(100, downloaded * 100 // total)
                    if percent != last_percent:
                        _print_download_progress(tier, downloaded, total, final=False)
                        last_percent = percent
                elif downloaded - last_byte_report >= _DOWNLOAD_LOG_BYTES:
                    _print_download_progress(tier, downloaded, total, final=False)
                    last_byte_report = downloaded
        if digest is not None and digest.hexdigest().lower() != sha256.lower():
            raise ModelResolverError(f"downloaded model failed sha256 verification: {target}")
        if total is None:
            if downloaded != last_byte_report:
                _print_download_progress(tier, downloaded, total, final=True)
            else:
                _finish_download_progress()
        elif last_percent != 100:
            _print_download_progress(tier, downloaded, total, final=True)
        else:
            _finish_download_progress()
        temp.replace(target)
    except Exception:
        _finish_download_progress()
        try:
            temp.unlink()
        except OSError:
            pass
        raise


def _response_content_length(response: Any) -> int | None:
    value = response.headers.get("content-length")
    if value is None:
        return None
    try:
        total = int(value, 10)
    except ValueError:
        return None
    return total if total > 0 else None


def _print_download_progress(
    tier: str,
    downloaded: int,
    total: int | None,
    *,
    final: bool,
) -> None:
    message = _download_progress_message(tier, downloaded, total)
    if sys.stderr.isatty():
        sys.stderr.write("\r" + message)
        if final:
            sys.stderr.write("\n")
        sys.stderr.flush()
        return
    if final or _should_log_download_progress(downloaded, total):
        print(message, file=sys.stderr, flush=True)


def _download_progress_message(tier: str, downloaded: int, total: int | None) -> str:
    if total is None:
        return f"Downloading {tier} model: {downloaded} bytes"
    percent = min(100, downloaded * 100 // total)
    return f"Downloading {tier} model: {downloaded}/{total} bytes ({percent}%)"


def _should_log_download_progress(downloaded: int, total: int | None) -> bool:
    if downloaded == 0:
        return True
    if total is not None and downloaded >= total:
        return True
    return downloaded % _DOWNLOAD_LOG_BYTES < _DOWNLOAD_CHUNK_BYTES


def _finish_download_progress() -> None:
    if sys.stderr.isatty():
        sys.stderr.write("\n")
        sys.stderr.flush()


def _ensure_snapshot_entry(snapshot_path: Path, blob_path: Path) -> None:
    snapshot_path.parent.mkdir(parents=True, exist_ok=True)
    if snapshot_path.exists() or snapshot_path.is_symlink():
        snapshot_path.unlink()

    try:
        relative_blob = os.path.relpath(blob_path, snapshot_path.parent)
        os.symlink(relative_blob, snapshot_path)
        return
    except OSError:
        pass

    try:
        os.link(blob_path, snapshot_path)
        return
    except OSError:
        pass

    shutil.copy2(blob_path, snapshot_path)


def _write_ref(repo: str, revision: str, commit: str) -> None:
    if not _COMMIT_RE.match(commit) or revision == commit:
        return
    ref = _repo_cache_dir(repo) / "refs" / revision
    ref.parent.mkdir(parents=True, exist_ok=True)
    temp = ref.with_name(ref.name + ".tmp")
    temp.write_text(commit + "\n", encoding="utf-8")
    temp.replace(ref)
