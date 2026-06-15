#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ATTEMORY_ROOT="${ATTEMORY_ROOT:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}"
ATMCORE_SDK="${ATMCORE_SDK:-}"

BUILD_ROOT="${BUILD_ROOT:-/tmp/attemory-manylinux2014-linux-cpu}"
ATTEMORY_BUILD_DIR="${ATTEMORY_BUILD_DIR:-${BUILD_ROOT}/attemory}"
DIST_DIR="${DIST_DIR:-${ATTEMORY_ROOT}/dist}"
RAW_DIST_DIR="${RAW_DIST_DIR:-${DIST_DIR}/raw-linux-cpu}"

MANYLINUX_PLAT="${MANYLINUX_PLAT:-manylinux2014_x86_64}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
PYTHON_BIN="${PYTHON_BIN:-}"
PYTHON_TAG="${PYTHON_TAG:-cp310-cp310}"
INSTALL_PYTHON_BUILD_DEPS="${INSTALL_PYTHON_BUILD_DEPS:-1}"
SMOKE_TEST="${SMOKE_TEST:-1}"
CLEAN_BUILD_ROOT="${CLEAN_BUILD_ROOT:-1}"
CLEAN_DIST="${CLEAN_DIST:-1}"
STRICT_MANYLINUX_BASELINE="${STRICT_MANYLINUX_BASELINE:-1}"
ATTEMORY_CMAKE_ARGS="${ATTEMORY_CMAKE_ARGS:-}"
HOST_UID="${HOST_UID:-}"
HOST_GID="${HOST_GID:-}"
atmcore_runtime_dirs=""

log() {
  printf '[linux-cpu-wheel] %s\n' "$*"
}

die() {
  printf '[linux-cpu-wheel] error: %s\n' "$*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || die "missing required file: $1"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

configure_linux_toolchain() {
  [[ "$(uname -s)" == "Linux" ]] || return
  if [[ -n "${CC:-}" && -n "${CXX:-}" ]]; then
    log "using C compiler from environment: ${CC}"
    log "using C++ compiler from environment: ${CXX}"
    return
  fi

  local roots=(
    /opt/rh/devtoolset-10/root/usr
    /opt/rh/gcc-toolset-10/root/usr
    /opt/rh/devtoolset-11/root/usr
    /opt/rh/gcc-toolset-11/root/usr
    /opt/rh/devtoolset-12/root/usr
    /opt/rh/gcc-toolset-12/root/usr
  )
  local root
  for root in "${roots[@]}"; do
    if [[ -x "${root}/bin/gcc" && -x "${root}/bin/g++" ]]; then
      export PATH="${root}/bin:${PATH}"
      export CC="${root}/bin/gcc"
      export CXX="${root}/bin/g++"
      log "using manylinux C compiler: ${CC}"
      log "using manylinux C++ compiler: ${CXX}"
      return
    fi
  done

  log "warning: no manylinux devtoolset compiler found; using CMake default compiler"
}

expand_path() {
  local path="$1"
  if [[ "${path}" == "~" ]]; then
    path="${HOME}"
  elif [[ "${path}" == "~/"* ]]; then
    path="${HOME}/${path#~/}"
  fi
  printf '%s\n' "${path}"
}

resolve_dir() {
  local path
  path="$(expand_path "$1")"
  [[ -d "${path}" ]] || die "missing directory: ${path}"
  (cd -- "${path}" && pwd)
}

select_python() {
  if [[ -n "${PYTHON_BIN}" ]]; then
    printf '%s\n' "${PYTHON_BIN}"
    return
  fi
  if [[ -x "/opt/python/${PYTHON_TAG}/bin/python" ]]; then
    printf '%s\n' "/opt/python/${PYTHON_TAG}/bin/python"
    return
  fi
  command -v python3 || command -v python || die "missing python"
}

cleanup_python_build_artifacts() {
  rm -rf \
    "${ATTEMORY_ROOT}/.pybuild" \
    "${ATTEMORY_ROOT}/python/attemory.egg-info" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/.pybuild" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/python/attemory_runtime_linux_cpu.egg-info"
}

restore_host_permissions() {
  if [[ -z "${HOST_UID}" || -z "${HOST_GID}" || "${HOST_UID}:${HOST_GID}" == "0:0" ]]; then
    if [[ "$(id -u)" != "0" ]]; then
      return
    fi
    local owner=""
    owner="$(stat -c '%u:%g' "${ATTEMORY_ROOT}" 2>/dev/null || stat -f '%u:%g' "${ATTEMORY_ROOT}" 2>/dev/null || true)"
    if [[ -n "${owner}" ]]; then
      HOST_UID="${owner%%:*}"
      HOST_GID="${owner##*:}"
    fi
  fi
  if [[ -z "${HOST_UID}" || -z "${HOST_GID}" ]]; then
    return
  fi
  if [[ "${HOST_UID}" == "0" && "${HOST_GID}" == "0" ]]; then
    return
  fi
  local paths=()
  local path
  for path in \
    "${DIST_DIR}" \
    "${ATTEMORY_ROOT}/.pybuild" \
    "${ATTEMORY_ROOT}/python/attemory.egg-info" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/.pybuild" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/python/attemory_runtime_linux_cpu.egg-info" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/python/attemory_runtime_linux_cpu"; do
    [[ -e "${path}" ]] && paths+=("${path}")
  done
  [[ "${#paths[@]}" -gt 0 ]] || return
  if ! chown -R "${HOST_UID}:${HOST_GID}" "${paths[@]}" 2>/dev/null; then
    log "warning: failed to restore artifact ownership"
  fi
}

finish() {
  cleanup_python_build_artifacts 2>/dev/null || true
  restore_host_permissions
}

run_cmake_configure() {
  local extra_args="$1"
  shift

  if [[ -n "${extra_args}" ]]; then
    # shellcheck disable=SC2206
    local parsed_extra_args=(${extra_args})
    cmake "$@" "${parsed_extra_args[@]}"
  else
    cmake "$@"
  fi
}

configure_atmcore_sdk() {
  [[ -n "${ATMCORE_SDK}" ]] || die "set ATMCORE_SDK=/path/to/attemory-core-sdk"
  ATMCORE_SDK="$(resolve_dir "${ATMCORE_SDK}")"
  require_file "${ATMCORE_SDK}/include/attemory-core/attemory-core.h"
  validate_atmcore_sdk_variant
  [[ -d "${ATMCORE_SDK}/lib" ]] || die "missing attemory-core SDK lib directory: ${ATMCORE_SDK}/lib"
  atmcore_runtime_dirs="${ATMCORE_SDK}/lib;${ATMCORE_SDK}/bin"
}

validate_atmcore_sdk_variant() {
  local build_info="${ATMCORE_SDK}/metadata/build-info.json"
  require_file "${build_info}"

  local sdk_variant
  sdk_variant="$("${PY}" - "${build_info}" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    print(json.load(f).get("variant", ""))
PY
)"
  [[ -n "${sdk_variant}" ]] || die "attemory-core SDK build-info.json does not contain variant"
  [[ "${sdk_variant}" == "linux-cpu" ]] || die "attemory-core SDK variant ${sdk_variant} does not match requested linux-cpu"
}

build_attemory() {
  log "configuring attemory: ${ATTEMORY_BUILD_DIR}"
  local cmake_args=(
    -S "${ATTEMORY_ROOT}" -B "${ATTEMORY_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DATMCORE_SDK="${ATMCORE_SDK}"
  )
  if [[ -n "${CC:-}" ]]; then
    cmake_args+=(-DCMAKE_C_COMPILER="${CC}")
  fi
  if [[ -n "${CXX:-}" ]]; then
    cmake_args+=(-DCMAKE_CXX_COMPILER="${CXX}")
  fi
  run_cmake_configure "${ATTEMORY_CMAKE_ARGS}" "${cmake_args[@]}"

  log "building attemory"
  cmake --build "${ATTEMORY_BUILD_DIR}" --target attemory_server --parallel "${JOBS}"
}

install_python_build_deps() {
  if [[ "${INSTALL_PYTHON_BUILD_DEPS}" != "1" ]]; then
    return
  fi
  log "installing Python build tools"
  "${PY}" -m pip install -U \
    "pip" \
    "setuptools>=70.1" \
    "build" \
    "auditwheel" \
    "cmake>=3.26" \
    "patchelf"
}

collect_runtime() {
  local runtime_dir
  local runtime_dir_args=()
  local IFS=';'
  for runtime_dir in ${atmcore_runtime_dirs}; do
    if [[ -d "${runtime_dir}" ]]; then
      runtime_dir_args+=(--runtime-dir "${runtime_dir}")
    fi
  done

  log "collecting runtime package contents"
  "${PY}" -B "${ATTEMORY_ROOT}/packaging/collect_runtime.py" \
    --variant linux-cpu \
    --server-binary "${ATTEMORY_BUILD_DIR}/bin/attemory_server" \
    "${runtime_dir_args[@]}" \
    --clean \
    --patch-rpath always
}

build_wheels() {
  mkdir -p "${DIST_DIR}" "${RAW_DIST_DIR}"
  if [[ "${CLEAN_DIST}" == "1" ]]; then
    rm -rf "${RAW_DIST_DIR}"
    mkdir -p "${RAW_DIST_DIR}"
    rm -f "${DIST_DIR}"/attemory_runtime_linux_cpu-*.whl
    rm -f "${DIST_DIR}"/attemory-*.whl
  fi

  cleanup_python_build_artifacts

  log "building runtime wheel"
  "${PY}" -m build --wheel \
    --outdir "${RAW_DIST_DIR}" \
    "${ATTEMORY_ROOT}/packaging/runtime-linux-cpu"

  local runtime_wheel
  runtime_wheel="$(ls -1 "${RAW_DIST_DIR}"/attemory_runtime_linux_cpu-*.whl | tail -n 1)"

  log "checking runtime wheel with auditwheel"
  auditwheel show "${runtime_wheel}"

  log "repairing runtime wheel for ${MANYLINUX_PLAT}"
  local runtime_lib_dir="${ATTEMORY_ROOT}/packaging/runtime-linux-cpu/python/attemory_runtime_linux_cpu/lib"
  LD_LIBRARY_PATH="${runtime_lib_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    auditwheel repair \
      --plat "${MANYLINUX_PLAT}" \
      --wheel-dir "${DIST_DIR}" \
      "${runtime_wheel}"

  log "building main Python wheel"
  "${PY}" -m build --wheel \
    --outdir "${DIST_DIR}" \
    "${ATTEMORY_ROOT}"

  cleanup_python_build_artifacts
}

current_glibc_version() {
  getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}'
}

manylinux_glibc_version() {
  local platform_tag="$1"
  case "${platform_tag}" in
    manylinux2014_*)
      printf '2.17\n'
      return
      ;;
    manylinux_2_17_*)
      printf '2.17\n'
      return
      ;;
  esac
  if [[ "${platform_tag}" =~ manylinux_([0-9]+)_([0-9]+)_ ]]; then
    printf '%s.%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
    return
  fi
  printf '0.0\n'
}

version_ge() {
  "${PY}" - "$1" "$2" <<'PY'
import sys

def parse(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split(".") if part)

raise SystemExit(0 if parse(sys.argv[1]) >= parse(sys.argv[2]) else 1)
PY
}

version_gt() {
  "${PY}" - "$1" "$2" <<'PY'
import sys

def parse(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in value.split(".") if part)

raise SystemExit(0 if parse(sys.argv[1]) > parse(sys.argv[2]) else 1)
PY
}

validate_manylinux_baseline() {
  local current_glibc
  local required_glibc

  current_glibc="$(current_glibc_version)"
  required_glibc="$(manylinux_glibc_version "${MANYLINUX_PLAT}")"
  if [[ "${required_glibc}" == "0.0" || -z "${current_glibc}" ]]; then
    log "unable to validate builder glibc for ${MANYLINUX_PLAT}; auditwheel remains the release gate"
    return
  fi

  log "builder glibc: ${current_glibc}; target glibc: ${required_glibc}"
  if version_gt "${current_glibc}" "${required_glibc}"; then
    local message="builder glibc is newer than ${MANYLINUX_PLAT}; use quay.io/pypa/manylinux2014_x86_64 for the default CPU wheel"
    if [[ "${STRICT_MANYLINUX_BASELINE}" == "1" ]]; then
      die "${message}"
    fi
    log "warning: ${message}"
  fi
}

select_smoke_runtime_wheel() {
  local repaired_runtime_wheel
  local raw_runtime_wheel
  local current_glibc
  local required_glibc

  repaired_runtime_wheel="$(ls -1 "${DIST_DIR}"/attemory_runtime_linux_cpu-*manylinux*.whl | tail -n 1)"
  raw_runtime_wheel="$(ls -1 "${RAW_DIST_DIR}"/attemory_runtime_linux_cpu-*.whl | tail -n 1)"

  current_glibc="$(current_glibc_version)"
  required_glibc="$(manylinux_glibc_version "${MANYLINUX_PLAT}")"
  if [[ -n "${current_glibc}" && "${required_glibc}" != "0.0" ]] && version_ge "${current_glibc}" "${required_glibc}"; then
    printf '%s\n' "${repaired_runtime_wheel}"
    return
  fi

  log "current glibc ${current_glibc:-unknown} is older than ${MANYLINUX_PLAT}; smoke testing raw linux_x86_64 runtime wheel" >&2
  log "final repaired wheel should be smoke tested on a glibc >= ${required_glibc} runner" >&2
  printf '%s\n' "${raw_runtime_wheel}"
}

smoke_test_wheels() {
  if [[ "${SMOKE_TEST}" != "1" ]]; then
    return
  fi

  local smoke_dir="${BUILD_ROOT}/smoke"
  local main_wheel
  local runtime_wheel
  main_wheel="$(ls -1 "${DIST_DIR}"/attemory-*.whl | tail -n 1)"
  runtime_wheel="$(select_smoke_runtime_wheel)"

  rm -rf "${smoke_dir}"
  log "running smoke test in ${smoke_dir}"
  log "smoke runtime wheel: ${runtime_wheel}"
  "${PY}" -m venv "${smoke_dir}"
  "${smoke_dir}/bin/python" -m pip install -U pip >/dev/null
  "${smoke_dir}/bin/python" -m pip install "${main_wheel}" "${runtime_wheel}" >/dev/null
  "${smoke_dir}/bin/attemory-server" --print-runtime-path
  "${smoke_dir}/bin/attemory-server" --help >/dev/null
}

main() {
  trap finish EXIT

  require_file "${ATTEMORY_ROOT}/pyproject.toml"

  PY="$(select_python)"
  export PATH="$(dirname "${PY}"):${PATH}"

  if [[ "${CLEAN_BUILD_ROOT}" == "1" ]]; then
    rm -rf "${BUILD_ROOT}"
  fi
  mkdir -p "${BUILD_ROOT}" "${DIST_DIR}"

  configure_linux_toolchain
  configure_atmcore_sdk
  log "attemory root: ${ATTEMORY_ROOT}"
  log "attemory-core SDK: ${ATMCORE_SDK}"
  log "python: ${PY}"
  log "manylinux platform: ${MANYLINUX_PLAT}"
  validate_manylinux_baseline

  install_python_build_deps
  require_command cmake
  require_command auditwheel
  require_command patchelf

  build_attemory
  collect_runtime
  build_wheels
  smoke_test_wheels

  log "done"
  log "wheels: ${DIST_DIR}"
}

main "$@"
