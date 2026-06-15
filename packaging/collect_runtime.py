#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

DEFAULT_OUTPUTS = {
    "linux-cpu": SCRIPT_DIR
    / "runtime-linux-cpu"
    / "python"
    / "attemory_runtime_linux_cpu",
    "macos-metal": SCRIPT_DIR
    / "runtime-macos-metal"
    / "python"
    / "attemory_runtime_macos_metal",
}
for cuda_tag in ("cu121", "cu124", "cu126", "cu129", "cu132"):
    DEFAULT_OUTPUTS[f"linux-cuda-{cuda_tag}"] = (
        SCRIPT_DIR
        / f"runtime-linux-cuda-{cuda_tag}"
        / "python"
        / f"attemory_runtime_linux_cuda_{cuda_tag}"
    )

CUDA_DRIVER_LIBRARY_NAMES = {
    "libcuda.so",
    "libcuda.so.1",
    "libnvidia-ml.so",
    "libnvidia-ml.so.1",
}
CUDA_MARKERS = (
    "cuda",
    "cublas",
    "cudart",
    "cufft",
    "curand",
    "cusolver",
    "cusparse",
    "nccl",
)

CUDA_TOOLKIT_ROOTS = tuple(
    Path(path).expanduser().resolve()
    for path in (
        os.environ.get("CUDA_HOME"),
        os.environ.get("CUDA_PATH"),
        "/usr/local/cuda",
    )
    if path and Path(path).expanduser().exists()
)
SYSTEM_BUILD_ROOTS = tuple(Path(path).resolve() for path in ("/", "/usr", "/usr/local"))
SYSTEM_LIBRARY_ROOTS = tuple(
    Path(path).resolve()
    for path in (
        "/lib",
        "/lib64",
        "/usr/lib",
        "/usr/lib64",
        "/usr/local/cuda",
        "/System/Library",
        "/Library/Developer",
        "/Applications/Xcode.app",
    )
    if Path(path).exists()
)


@dataclass(frozen=True)
class Dependency:
    name: str
    path: Path


@dataclass(frozen=True)
class CollectionPlan:
    server_binary: Path
    output_root: Path
    runtime_dirs: tuple[Path, ...]
    copied_libs: tuple[Dependency, ...]
    external_deps: tuple[Dependency, ...]
    missing_deps: tuple[str, ...]
    cuda_deps: tuple[str, ...]


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        plan = build_collection_plan(args)
        validate_plan(args, plan)
        if args.dry_run:
            print_plan(plan, dry_run=True)
            return 0
        collect_runtime(args, plan)
        print_plan(plan, dry_run=False)
        return 0
    except RuntimeCollectorError as exc:
        print(f"collect-runtime: error: {exc}", file=sys.stderr)
        return 1


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Collect a native attemory runtime into a runtime package tree.",
    )
    parser.add_argument(
        "--variant",
        default="linux-cpu",
        choices=sorted(DEFAULT_OUTPUTS),
        help="runtime variant to collect",
    )
    parser.add_argument(
        "--prefix",
        type=Path,
        help="install or build prefix containing bin/ and lib/ directories",
    )
    parser.add_argument(
        "--server-binary",
        type=Path,
        help="attemory_server path; defaults to PREFIX/bin/attemory_server",
    )
    parser.add_argument(
        "--runtime-dir",
        action="append",
        type=Path,
        default=[],
        help="directory containing project-built shared libraries; may be repeated",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="runtime package module root to populate",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="clean output bin/, lib/, and metadata/build-info.json before copying",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the collection plan without copying files",
    )
    parser.add_argument(
        "--patch-rpath",
        choices=("auto", "always", "never"),
        default="auto",
        help="set runtime RPATH with patchelf after copying",
    )
    return parser.parse_args(argv)


def build_collection_plan(args: argparse.Namespace) -> CollectionPlan:
    require_supported_platform(args.variant)
    prefix = normalize_optional_path(args.prefix)
    server_binary = resolve_server_binary(prefix, args.server_binary)
    output_root = resolve_output_root(args.variant, args.output)
    runtime_dirs = resolve_runtime_dirs(prefix, server_binary, args.runtime_dir)

    copied_by_name: dict[str, Dependency] = {}
    external_by_path: dict[Path, Dependency] = {}
    missing: set[str] = set()
    cuda_deps: set[str] = set()

    queue: list[Path] = [server_binary]
    seen: set[Path] = set()
    while queue:
        current = queue.pop(0).resolve()
        if current in seen:
            continue
        seen.add(current)
        deps, unresolved = read_dependencies(args.variant, current, runtime_dirs)
        missing.update(unresolved)

        for dep in deps:
            if contains_cuda_marker(dep.name) or contains_cuda_marker(str(dep.path)):
                cuda_deps.add(f"{dep.name} => {dep.path}")

            if args.variant.startswith("linux-cuda-") and is_cuda_driver_library(dep.name):
                external_by_path[dep.path.resolve()] = Dependency(
                    dep.name,
                    dep.path.resolve(),
                )
                continue

            if is_under_any(dep.path, runtime_dirs) and not is_system_library(
                args.variant,
                dep.path,
            ):
                copied = Dependency(dep.name, dep.path.resolve())
                record_copied_dependency(copied_by_name, copied)
                if copied.path not in seen:
                    queue.append(copied.path)
            else:
                external_by_path[dep.path.resolve()] = Dependency(
                    dep.name,
                    dep.path.resolve(),
                )

    return CollectionPlan(
        server_binary=server_binary,
        output_root=output_root,
        runtime_dirs=tuple(runtime_dirs),
        copied_libs=tuple(sorted(copied_by_name.values(), key=lambda dep: dep.name)),
        external_deps=tuple(
            sorted(external_by_path.values(), key=lambda dep: (dep.name, str(dep.path)))
        ),
        missing_deps=tuple(sorted(missing)),
        cuda_deps=tuple(sorted(cuda_deps)),
    )


def record_copied_dependency(
    copied_by_name: dict[str, Dependency],
    dependency: Dependency,
) -> None:
    existing = copied_by_name.get(dependency.name)
    if existing is not None and existing.path != dependency.path:
        raise RuntimeCollectorError(
            "conflicting runtime libraries with the same filename: "
            f"{dependency.name} from {existing.path} and {dependency.path}"
        )
    copied_by_name[dependency.name] = dependency


def validate_plan(args: argparse.Namespace, plan: CollectionPlan) -> None:
    missing_deps = tuple(
        dep
        for dep in plan.missing_deps
        if not (args.variant.startswith("linux-cuda-") and is_cuda_driver_library(dep))
    )
    if missing_deps:
        missing = ", ".join(missing_deps)
        raise RuntimeCollectorError(f"unresolved shared libraries: {missing}")
    if args.variant == "linux-cpu" and plan.cuda_deps:
        deps = "; ".join(plan.cuda_deps)
        raise RuntimeCollectorError(
            "linux-cpu runtime cannot contain CUDA dependencies. "
            f"Found: {deps}"
        )


def collect_runtime(args: argparse.Namespace, plan: CollectionPlan) -> None:
    output_root = plan.output_root
    bin_dir = output_root / "bin"
    lib_dir = output_root / "lib"
    metadata_dir = output_root / "metadata"

    if args.clean:
        clean_output(output_root)

    bin_dir.mkdir(parents=True, exist_ok=True)
    lib_dir.mkdir(parents=True, exist_ok=True)
    metadata_dir.mkdir(parents=True, exist_ok=True)

    server_dst = bin_dir / "attemory_server"
    copy_executable(plan.server_binary, server_dst)
    copied_paths = [server_dst]

    for dep in plan.copied_libs:
        dst = lib_dir / dep.name
        copy_file(dep.path, dst)
        copied_paths.append(dst)

    ensure_runtime_library_aliases(args.variant, plan.runtime_dirs, lib_dir, copied_paths)

    patch_info = patch_runtime_paths(
        args.variant,
        args.patch_rpath,
        server_dst,
        copied_paths[1:],
    )
    write_build_info(args, plan, patch_info)


def read_dependencies(
    variant: str,
    binary: Path,
    runtime_dirs: Iterable[Path],
) -> tuple[list[Dependency], list[str]]:
    if variant.startswith("linux-"):
        return read_ldd(binary, runtime_dirs)
    if variant.startswith("macos-"):
        return read_otool(binary, runtime_dirs)
    raise RuntimeCollectorError(f"unsupported runtime variant: {variant}")


def read_ldd(binary: Path, runtime_dirs: Iterable[Path]) -> tuple[list[Dependency], list[str]]:
    if not binary.exists():
        raise RuntimeCollectorError(f"missing binary: {binary}")

    env = os.environ.copy()
    runtime_path = os.pathsep.join(str(path) for path in runtime_dirs)
    if runtime_path:
        existing = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = runtime_path if not existing else runtime_path + os.pathsep + existing

    completed = subprocess.run(
        ["ldd", str(binary)],
        check=False,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        message = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeCollectorError(f"ldd failed for {binary}: {message}")

    deps: list[Dependency] = []
    unresolved_names: list[str] = []
    for line in completed.stdout.splitlines():
        parsed = parse_ldd_line(line)
        if parsed is None:
            continue
        name, path = parsed
        if path is None:
            unresolved_names.append(name)
            continue
        deps.append(Dependency(name=name, path=path))

    missing: list[str] = []
    for name in unresolved_names:
        recovered = find_dependency_by_soname(name, runtime_dirs)
        if recovered is None:
            missing.append(name)
        else:
            deps.append(Dependency(name=name, path=recovered))
    return deps, missing


def read_otool(
    binary: Path,
    runtime_dirs: Iterable[Path],
) -> tuple[list[Dependency], list[str]]:
    if not binary.exists():
        raise RuntimeCollectorError(f"missing binary: {binary}")

    completed = subprocess.run(
        ["otool", "-L", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        message = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeCollectorError(f"otool failed for {binary}: {message}")

    deps: list[Dependency] = []
    missing: list[str] = []
    for install_name in parse_otool_output(completed.stdout):
        if is_macos_system_install_name(install_name):
            resolved = resolve_existing_absolute_install_name(install_name)
            if resolved is not None:
                deps.append(Dependency(name=install_name, path=resolved))
            continue

        resolved = resolve_macho_dependency(install_name, binary, runtime_dirs)
        if resolved is None:
            missing.append(install_name)
        else:
            if resolved == binary.resolve():
                continue
            deps.append(Dependency(name=Path(install_name).name, path=resolved))
    return deps, missing


def parse_otool_output(output: str) -> list[str]:
    install_names: list[str] = []
    for index, line in enumerate(output.splitlines()):
        if index == 0:
            continue
        stripped = line.strip()
        if not stripped:
            continue
        install_name = stripped.split(" ", 1)[0]
        if install_name:
            install_names.append(install_name)
    return install_names


def resolve_macho_dependency(
    install_name: str,
    binary: Path,
    runtime_dirs: Iterable[Path],
) -> Path | None:
    absolute = resolve_existing_absolute_install_name(install_name)
    if absolute is not None:
        return absolute

    suffix: str | None = None
    for prefix in ("@rpath/", "@loader_path/", "@executable_path/"):
        if install_name.startswith(prefix):
            suffix = install_name[len(prefix) :]
            break
    if suffix is None:
        suffix = Path(install_name).name

    if install_name.startswith("@loader_path/"):
        candidate = (binary.parent / suffix).resolve()
        if candidate.exists():
            return candidate
    if install_name.startswith("@executable_path/"):
        candidate = (binary.parent / suffix).resolve()
        if candidate.exists():
            return candidate

    name = Path(suffix).name
    for root in runtime_dirs:
        direct = (root / suffix).resolve()
        if direct.exists():
            return direct
        by_name = (root / name).resolve()
        if by_name.exists():
            return by_name
    return None


def resolve_existing_absolute_install_name(install_name: str) -> Path | None:
    if not install_name.startswith("/"):
        return None
    path = Path(install_name)
    return path.resolve() if path.exists() else None


def parse_ldd_line(line: str) -> tuple[str, Path | None] | None:
    line = line.strip()
    if not line or line.startswith("linux-vdso"):
        return None

    not_found = re.match(r"(?P<name>\S+)\s+=>\s+not found$", line)
    if not_found:
        return not_found.group("name"), None

    linked = re.match(r"(?P<name>\S+)\s+=>\s+(?P<path>/\S+)\s+\(", line)
    if linked:
        return linked.group("name"), Path(linked.group("path"))

    direct = re.match(r"(?P<path>/\S+)\s+\(", line)
    if direct:
        path = Path(direct.group("path"))
        return path.name, path

    return None


def find_dependency_by_soname(name: str, runtime_dirs: Iterable[Path]) -> Path | None:
    for root in runtime_dirs:
        direct = root / name
        if direct.exists():
            return direct

    for root in runtime_dirs:
        for candidate in sorted(root.glob("*.so*")):
            if not candidate.is_file():
                continue
            if read_elf_soname(candidate) == name:
                return candidate
    return None


def read_elf_soname(path: Path) -> str | None:
    completed = subprocess.run(
        ["readelf", "-d", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if completed.returncode != 0:
        return None
    for line in completed.stdout.splitlines():
        match = re.search(r"\(SONAME\)\s+Library soname: \[(?P<soname>[^\]]+)\]", line)
        if match:
            return match.group("soname")
    return None


def patch_rpaths(
    mode: str,
    server_binary: Path,
    copied_libs: Iterable[Path],
) -> dict[str, object]:
    if mode == "never":
        return {"mode": mode, "patched": False, "reason": "disabled"}

    patchelf = shutil.which("patchelf")
    if patchelf is None:
        if mode == "always":
            raise RuntimeCollectorError("patchelf is required but was not found")
        return {"mode": mode, "patched": False, "reason": "patchelf not found"}

    run_command([patchelf, "--set-rpath", "$ORIGIN/../lib", str(server_binary)])
    for lib in copied_libs:
        run_command([patchelf, "--set-rpath", "$ORIGIN", str(lib)])
    return {"mode": mode, "patched": True, "tool": patchelf}


def patch_runtime_paths(
    variant: str,
    mode: str,
    server_binary: Path,
    copied_libs: Iterable[Path],
) -> dict[str, object]:
    if variant.startswith("linux-"):
        return patch_rpaths(mode, server_binary, copied_libs)
    if variant.startswith("macos-"):
        return patch_macos_install_names(mode, server_binary, copied_libs)
    raise RuntimeCollectorError(f"unsupported runtime variant: {variant}")


def patch_macos_install_names(
    mode: str,
    server_binary: Path,
    copied_libs: Iterable[Path],
) -> dict[str, object]:
    if mode == "never":
        return {"mode": mode, "patched": False, "reason": "disabled"}

    install_name_tool = shutil.which("install_name_tool")
    if install_name_tool is None:
        if mode == "always":
            raise RuntimeCollectorError("install_name_tool is required but was not found")
        return {"mode": mode, "patched": False, "reason": "install_name_tool not found"}

    copied = tuple(copied_libs)
    copied_names = {path.name for path in copied}

    ensure_macho_rpath(install_name_tool, server_binary, "@loader_path/../lib")
    for lib in copied:
        ensure_macho_rpath(install_name_tool, lib, "@loader_path")
        run_command([install_name_tool, "-id", f"@rpath/{lib.name}", str(lib)])

    for target in (server_binary, *copied):
        for install_name in read_raw_macho_install_names(target):
            dep_name = Path(install_name).name
            if dep_name not in copied_names:
                continue
            replacement = (
                f"@loader_path/../lib/{dep_name}"
                if target == server_binary
                else f"@loader_path/{dep_name}"
            )
            run_command([install_name_tool, "-change", install_name, replacement, str(target)])

    codesign = shutil.which("codesign")
    signed = False
    if codesign is not None:
        for target in (server_binary, *copied):
            run_command([codesign, "--force", "--sign", "-", str(target)])
        signed = True

    return {
        "mode": mode,
        "patched": True,
        "tool": install_name_tool,
        "codesign": signed,
    }


def ensure_macho_rpath(tool: str, binary: Path, rpath: str) -> None:
    if rpath in read_macho_rpaths(binary):
        return
    run_command([tool, "-add_rpath", rpath, str(binary)])


def read_raw_macho_install_names(binary: Path) -> tuple[str, ...]:
    completed = subprocess.run(
        ["otool", "-L", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeCollectorError(f"otool failed for {binary}: {detail}")
    return tuple(parse_otool_output(completed.stdout))


def read_macho_rpaths(binary: Path) -> tuple[str, ...]:
    completed = subprocess.run(
        ["otool", "-l", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        return ()

    rpaths: list[str] = []
    lines = completed.stdout.splitlines()
    for index, line in enumerate(lines):
        if line.strip() != "cmd LC_RPATH":
            continue
        for detail in lines[index + 1 : index + 5]:
            stripped = detail.strip()
            if stripped.startswith("path "):
                rpaths.append(stripped.split(" ", 2)[1])
                break
    return tuple(rpaths)


def write_build_info(
    args: argparse.Namespace,
    plan: CollectionPlan,
    patch_info: dict[str, object],
) -> None:
    core_version, core_api_version = read_atmcore_version(plan.runtime_dirs)
    info = {
        "runtime": args.variant,
        "attemory_version": read_attemory_version(),
        "attemory_core_version": core_version,
        "attemory_core_api_version": core_api_version,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "server_binary": str(plan.server_binary),
        "runtime_dirs": [str(path) for path in plan.runtime_dirs],
        "copied_libraries": [
            {"name": dep.name, "path": str(dep.path)} for dep in plan.copied_libs
        ],
        "external_dependencies": [
            {"name": dep.name, "path": str(dep.path)} for dep in plan.external_deps
        ],
        "rpath": patch_info,
    }
    metadata_dir = plan.output_root / "metadata"
    metadata_dir.mkdir(parents=True, exist_ok=True)
    write_json(metadata_dir / "build-info.json", info)


def read_attemory_version() -> str:
    version_file = REPO_ROOT / "VERSION"
    try:
        version = version_file.read_text(encoding="utf-8").splitlines()[0].strip()
    except (IndexError, OSError) as exc:
        raise RuntimeCollectorError(f"failed to read attemory version: {version_file}") from exc
    if not version:
        raise RuntimeCollectorError(f"empty attemory version: {version_file}")
    return version


def read_atmcore_version(runtime_dirs: Iterable[Path]) -> tuple[str, int]:
    header = find_atmcore_version_header(runtime_dirs)
    version = read_c_string_define(header, "ATMCORE_VERSION_STRING")
    api_version = read_c_int_define(header, "ATMCORE_API_VERSION")
    return version, api_version


def find_atmcore_version_header(runtime_dirs: Iterable[Path]) -> Path:
    candidates: list[Path] = []
    env_sdk = os.environ.get("ATMCORE_SDK")
    if env_sdk:
        candidates.append(Path(env_sdk).expanduser() / "include" / "attemory-core" / "version.h")
    for runtime_dir in runtime_dirs:
        candidates.append(runtime_dir.parent / "include" / "attemory-core" / "version.h")

    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        if resolved.exists():
            return resolved
    raise RuntimeCollectorError("failed to find attemory-core version header in ATMCORE_SDK or runtime dirs")


def read_c_string_define(path: Path, name: str) -> str:
    pattern = re.compile(rf'^#define\s+{re.escape(name)}\s+"([^"]+)"')
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    raise RuntimeCollectorError(f"{path} does not define {name}")


def read_c_int_define(path: Path, name: str) -> int:
    pattern = re.compile(rf"^#define\s+{re.escape(name)}\s+([0-9]+)")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return int(match.group(1))
    raise RuntimeCollectorError(f"{path} does not define {name}")


def print_plan(plan: CollectionPlan, *, dry_run: bool) -> None:
    summary = {
        "dry_run": dry_run,
        "server_binary": str(plan.server_binary),
        "output_root": str(plan.output_root),
        "runtime_dirs": [str(path) for path in plan.runtime_dirs],
        "copied_libraries": [
            {"name": dep.name, "path": str(dep.path)} for dep in plan.copied_libs
        ],
        "external_dependencies": [
            {"name": dep.name, "path": str(dep.path)} for dep in plan.external_deps
        ],
    }
    print(json.dumps(summary, indent=2, sort_keys=True))


def clean_output(output_root: Path) -> None:
    for relative in ("bin", "lib"):
        path = output_root / relative
        if path.exists():
            shutil.rmtree(path)
    build_info = output_root / "metadata" / "build-info.json"
    if build_info.exists():
        build_info.unlink()


def resolve_server_binary(prefix: Path | None, server_binary: Path | None) -> Path:
    if server_binary is not None:
        return server_binary.expanduser().resolve()
    if prefix is None:
        raise RuntimeCollectorError("provide --prefix or --server-binary")
    return (prefix / "bin" / "attemory_server").resolve()


def resolve_output_root(variant: str, output: Path | None) -> Path:
    if output is not None:
        return output.expanduser().resolve()
    try:
        return DEFAULT_OUTPUTS[variant].resolve()
    except KeyError as exc:
        raise RuntimeCollectorError(f"unsupported runtime variant: {variant}") from exc


def resolve_runtime_dirs(
    prefix: Path | None,
    server_binary: Path,
    explicit_dirs: Iterable[Path],
) -> tuple[Path, ...]:
    dirs: list[Path] = []
    if prefix is not None:
        dirs.extend([prefix / "lib", prefix / "lib64", prefix / "bin"])

    build_root = server_binary.parent.parent
    if not is_system_build_root(build_root):
        dirs.extend([build_root / "lib", build_root / "lib64", build_root / "bin"])

    dirs.extend(explicit_dirs)
    normalized = []
    seen: set[Path] = set()
    for path in dirs:
        resolved = path.expanduser().resolve()
        if resolved in seen or not resolved.exists():
            continue
        seen.add(resolved)
        normalized.append(resolved)
    return tuple(normalized)


def normalize_optional_path(path: Path | None) -> Path | None:
    return path.expanduser().resolve() if path is not None else None


def require_supported_platform(variant: str) -> None:
    system = platform.system()
    if variant.startswith("linux-") and system != "Linux":
        raise RuntimeCollectorError(f"{variant} collection must run on Linux")
    if variant.startswith("macos-") and system != "Darwin":
        raise RuntimeCollectorError(f"{variant} collection must run on macOS")


def copy_file(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_executable(src: Path, dst: Path) -> None:
    copy_file(src, dst)
    mode = dst.stat().st_mode
    dst.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def ensure_runtime_library_aliases(
    variant: str,
    runtime_dirs: Iterable[Path],
    lib_dir: Path,
    copied_paths: list[Path],
) -> None:
    if variant != "macos-metal":
        return
    ensure_runtime_library_alias(
        "libattemory-core.dylib",
        runtime_dirs,
        lib_dir,
        copied_paths,
    )


def ensure_runtime_library_alias(
    expected_name: str,
    runtime_dirs: Iterable[Path],
    lib_dir: Path,
    copied_paths: list[Path],
) -> None:
    dst = lib_dir / expected_name
    if dst.exists():
        if dst not in copied_paths:
            copied_paths.append(dst)
        return

    source = find_runtime_library(expected_name, runtime_dirs)
    if source is None:
        raise RuntimeCollectorError(
            f"failed to find required runtime library: {expected_name}"
        )
    copy_file(source, dst)
    copied_paths.append(dst)


def find_runtime_library(name: str, runtime_dirs: Iterable[Path]) -> Path | None:
    stem = name.removesuffix(".dylib")
    for root in runtime_dirs:
        exact = root / name
        if exact.exists():
            return exact
    for root in runtime_dirs:
        candidates = sorted(root.glob(f"{stem}*.dylib"))
        for candidate in candidates:
            if candidate.is_file() or candidate.is_symlink():
                return candidate
    return None


def run_command(command: list[str]) -> None:
    completed = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeCollectorError(f"{command[0]} failed: {detail}")


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def is_under_any(path: Path, roots: Iterable[Path]) -> bool:
    resolved = path.resolve()
    for root in roots:
        try:
            resolved.relative_to(root.resolve())
            return True
        except ValueError:
            continue
    return False


def is_system_build_root(path: Path) -> bool:
    resolved = path.resolve()
    return resolved in SYSTEM_BUILD_ROOTS


def is_system_library(variant: str, path: Path) -> bool:
    if variant.startswith("linux-cuda-") and is_cuda_driver_library(path.name):
        return True
    if variant.startswith("linux-cuda-") and is_under_any(path, CUDA_TOOLKIT_ROOTS):
        return is_cuda_driver_library(path.name)
    return is_under_any(path, SYSTEM_LIBRARY_ROOTS)


def is_cuda_driver_library(name: str) -> bool:
    return name in CUDA_DRIVER_LIBRARY_NAMES


def is_macos_system_install_name(install_name: str) -> bool:
    return (
        install_name.startswith("/usr/lib/")
        or install_name.startswith("/System/Library/")
        or install_name.startswith("/Library/Developer/")
        or install_name.startswith("/Applications/Xcode.app/")
    )


def contains_cuda_marker(value: str) -> bool:
    lowered = value.lower()
    return any(marker in lowered for marker in CUDA_MARKERS)


class RuntimeCollectorError(Exception):
    pass


if __name__ == "__main__":
    raise SystemExit(main())
