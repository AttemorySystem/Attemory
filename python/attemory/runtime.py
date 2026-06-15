from __future__ import annotations

import importlib
import os
import platform
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DIRECT_BINARY_ENV = "ATTEMORY_SERVER_BINARY"


@dataclass(frozen=True)
class RuntimeCandidate:
    name: str
    package: str
    platforms: tuple[str, ...]


@dataclass(frozen=True)
class RuntimeInfo:
    name: str
    package: str
    server_binary: Path
    lib_dir: Path | None = None
    metadata_dir: Path | None = None


RUNTIME_CANDIDATES: tuple[RuntimeCandidate, ...] = (
    RuntimeCandidate(
        name="linux-cuda-cu132",
        package="attemory_runtime_linux_cuda_cu132",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="linux-cuda-cu129",
        package="attemory_runtime_linux_cuda_cu129",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="linux-cuda-cu126",
        package="attemory_runtime_linux_cuda_cu126",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="linux-cuda-cu124",
        package="attemory_runtime_linux_cuda_cu124",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="linux-cuda-cu121",
        package="attemory_runtime_linux_cuda_cu121",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="linux-cpu",
        package="attemory_runtime_linux_cpu",
        platforms=("Linux",),
    ),
    RuntimeCandidate(
        name="macos-metal",
        package="attemory_runtime_macos_metal",
        platforms=("Darwin",),
    ),
    RuntimeCandidate(
        name="macos-cpu",
        package="attemory_runtime_macos_cpu",
        platforms=("Darwin",),
    ),
    RuntimeCandidate(
        name="windows-cpu",
        package="attemory_runtime_windows_cpu",
        platforms=("Windows",),
    ),
)


def resolve_runtime() -> RuntimeInfo:
    direct_binary = os.environ.get(DIRECT_BINARY_ENV)
    if direct_binary:
        return RuntimeInfo(
            name="direct",
            package="env",
            server_binary=Path(direct_binary).expanduser(),
        )

    candidates = list(_platform_candidates())
    missing: list[str] = []
    unavailable: list[str] = []
    for candidate in candidates:
        info = _load_runtime_info(candidate)
        if info is None:
            missing.append(candidate.package)
            continue
        if info.server_binary.exists():
            return info
        unavailable.append(f"{candidate.package}:{info.server_binary}")

    details = ", ".join(missing + unavailable)
    if not details:
        details = "no runtime candidate matches this platform"
    raise RuntimeError(
        "no installed attemory native runtime was found; install a package such as "
        "attemory-runtime-linux-cpu, attemory-runtime-linux-cuda-cu126, or set "
        f"{DIRECT_BINARY_ENV}=PATH. Checked: {details}"
    )


def runtime_environment(info: RuntimeInfo) -> dict[str, str]:
    env = dict(os.environ)
    if info.lib_dir is None or not info.lib_dir.exists():
        return env

    if platform.system() == "Windows":
        key = "PATH"
    elif platform.system() == "Darwin":
        key = "DYLD_LIBRARY_PATH"
    else:
        key = "LD_LIBRARY_PATH"

    existing = env.get(key)
    lib_dir = str(info.lib_dir)
    env[key] = lib_dir if not existing else lib_dir + os.pathsep + existing
    return env


def _platform_candidates() -> tuple[RuntimeCandidate, ...]:
    system = platform.system()
    return tuple(
        candidate
        for candidate in RUNTIME_CANDIDATES
        if system in candidate.platforms
    )


def _load_runtime_info(candidate: RuntimeCandidate) -> RuntimeInfo | None:
    try:
        module = importlib.import_module(candidate.package)
    except ModuleNotFoundError:
        return None

    raw_info: Any
    if hasattr(module, "runtime_info"):
        raw_info = module.runtime_info()
    else:
        raw_info = {}

    if isinstance(raw_info, RuntimeInfo):
        return raw_info
    if not isinstance(raw_info, dict):
        raw_info = {}

    server_binary = raw_info.get("server_binary")
    if not server_binary:
        package_root = Path(module.__file__).resolve().parent
        executable = "attemory_server.exe" if platform.system() == "Windows" else "attemory_server"
        server_binary = package_root / "bin" / executable

    lib_dir = raw_info.get("lib_dir")
    metadata_dir = raw_info.get("metadata_dir")
    return RuntimeInfo(
        name=str(raw_info.get("name") or candidate.name),
        package=candidate.package,
        server_binary=Path(server_binary).expanduser(),
        lib_dir=Path(lib_dir).expanduser() if lib_dir else None,
        metadata_dir=Path(metadata_dir).expanduser() if metadata_dir else None,
    )
