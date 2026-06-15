#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ATTEMORY_ROOT="${ATTEMORY_ROOT:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}"
ATMCORE_SDK="${ATMCORE_SDK:-}"

BUILD_ROOT="${BUILD_ROOT:-/tmp/attemory-macos-metal}"
ATTEMORY_BUILD_DIR="${ATTEMORY_BUILD_DIR:-${BUILD_ROOT}/attemory}"
BUILD_VENV_DIR="${BUILD_VENV_DIR:-${BUILD_ROOT}/build-venv}"
DIST_DIR="${DIST_DIR:-${ATTEMORY_ROOT}/dist}"
RAW_DIST_DIR="${RAW_DIST_DIR:-${DIST_DIR}/raw-macos-metal}"

BUILD_TYPE="${BUILD_TYPE:-Release}"
MACOS_ARCH="${MACOS_ARCH:-arm64}"
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
PYTHON_BIN="${PYTHON_BIN:-}"
INSTALL_PYTHON_BUILD_DEPS="${INSTALL_PYTHON_BUILD_DEPS:-1}"
SMOKE_TEST="${SMOKE_TEST:-1}"
CLEAN_BUILD_ROOT="${CLEAN_BUILD_ROOT:-1}"
CLEAN_DIST="${CLEAN_DIST:-1}"
ATTEMORY_CMAKE_ARGS="${ATTEMORY_CMAKE_ARGS:-}"
BASE_PY=""
PY=""
atmcore_runtime_dirs=""

log() {
  printf '[macos-metal-wheel] %s\n' "$*"
}

die() {
  printf '[macos-metal-wheel] error: %s\n' "$*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || die "missing required file: $1"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
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
  local candidate
  if [[ -n "${PYTHON_BIN}" ]]; then
    validate_python_version "${PYTHON_BIN}"
    printf '%s\n' "${PYTHON_BIN}"
    return
  fi

  for candidate in python3.13 python3.12 python3.11 python3.10 python3.9 python3 python; do
    if ! command -v "${candidate}" >/dev/null 2>&1; then
      continue
    fi
    candidate="$(command -v "${candidate}")"
    if "${candidate}" - <<'PY' >/dev/null 2>&1
import sys
raise SystemExit(0 if sys.version_info >= (3, 9) else 1)
PY
    then
      printf '%s\n' "${candidate}"
      return
    fi
  done

  die "missing Python >= 3.9; set PYTHON_BIN=/path/to/python3.9+"
}

validate_python_version() {
  "$1" - <<'PY' || die "Python >= 3.9 is required; got $("$1" -c 'import sys; print(".".join(map(str, sys.version_info[:3])))')"
import sys
raise SystemExit(0 if sys.version_info >= (3, 9) else 1)
PY
}

cleanup_python_build_artifacts() {
  rm -rf \
    "${ATTEMORY_ROOT}/.pybuild" \
    "${ATTEMORY_ROOT}/python/attemory.egg-info" \
    "${ATTEMORY_ROOT}/packaging/runtime-macos-metal/.pybuild" \
    "${ATTEMORY_ROOT}/packaging/runtime-macos-metal/python/attemory_runtime_macos_metal.egg-info"
}

finish() {
  cleanup_python_build_artifacts 2>/dev/null || true
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

require_macos() {
  local system
  system="$(uname -s)"
  [[ "${system}" == "Darwin" ]] || die "macOS Metal wheel must be built on macOS"
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
  sdk_variant="$("${BASE_PY}" - "${build_info}" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    print(json.load(f).get("variant", ""))
PY
)"
  [[ -n "${sdk_variant}" ]] || die "attemory-core SDK build-info.json does not contain variant"
  [[ "${sdk_variant}" == "macos-metal" ]] || die "attemory-core SDK variant ${sdk_variant} does not match requested macos-metal"
}

build_attemory() {
  log "configuring attemory: ${ATTEMORY_BUILD_DIR}"
  run_cmake_configure "${ATTEMORY_CMAKE_ARGS}" \
    -S "${ATTEMORY_ROOT}" -B "${ATTEMORY_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_OSX_ARCHITECTURES="${MACOS_ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET}" \
    -DATMCORE_SDK="${ATMCORE_SDK}"

  log "building attemory"
  cmake --build "${ATTEMORY_BUILD_DIR}" --target attemory_server --parallel "${JOBS}"
}

prepare_python_build_env() {
  PY="${BASE_PY}"

  if [[ "${INSTALL_PYTHON_BUILD_DEPS}" != "1" ]]; then
    export PATH="$(dirname "${PY}"):${PATH}"
    log "build python: ${PY}"
    return
  fi

  log "creating Python build venv: ${BUILD_VENV_DIR}"
  "${BASE_PY}" -m venv "${BUILD_VENV_DIR}"
  PY="${BUILD_VENV_DIR}/bin/python"
  validate_python_version "${PY}"
  export PATH="$(dirname "${PY}"):${PATH}"
  log "build python: ${PY}"

  log "installing Python build tools"
  "${PY}" -m pip install -U \
    "pip" \
    "setuptools>=70.1" \
    "build" \
    "wheel" \
    "delocate"
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
    --variant macos-metal \
    --server-binary "${ATTEMORY_BUILD_DIR}/bin/attemory_server" \
    "${runtime_dir_args[@]}" \
    --clean \
    --patch-rpath always

  require_file \
    "${ATTEMORY_ROOT}/packaging/runtime-macos-metal/python/attemory_runtime_macos_metal/lib/libattemory-core.dylib"
}

build_wheels() {
  mkdir -p "${DIST_DIR}" "${RAW_DIST_DIR}"
  if [[ "${CLEAN_DIST}" == "1" ]]; then
    rm -rf "${RAW_DIST_DIR}"
    mkdir -p "${RAW_DIST_DIR}"
    rm -f "${DIST_DIR}"/attemory_runtime_macos_metal-*.whl
    rm -f "${DIST_DIR}"/attemory-*.whl
  fi

  cleanup_python_build_artifacts

  log "building macOS Metal runtime wheel"
  "${PY}" -m build --wheel \
    --outdir "${RAW_DIST_DIR}" \
    "${ATTEMORY_ROOT}/packaging/runtime-macos-metal"

  local runtime_wheel
  runtime_wheel="$(ls -1 "${RAW_DIST_DIR}"/attemory_runtime_macos_metal-*.whl | tail -n 1)"
  "${PY}" - "${runtime_wheel}" <<'PY'
import sys
import zipfile

wheel = sys.argv[1]
required = "attemory_runtime_macos_metal/lib/libattemory-core.dylib"
with zipfile.ZipFile(wheel) as zf:
    names = set(zf.namelist())
if required not in names:
    raise SystemExit(f"raw runtime wheel is missing required file: {required}")
PY

  log "checking raw runtime wheel with delocate-listdeps"
  delocate-listdeps --all "${runtime_wheel}"

  log "repairing macOS runtime wheel with delocate-wheel"
  delocate-wheel \
    --require-archs "${MACOS_ARCH}" \
    --wheel-dir "${DIST_DIR}" \
    "${runtime_wheel}"

  local repaired_runtime_wheel
  repaired_runtime_wheel="$(ls -1 "${DIST_DIR}"/attemory_runtime_macos_metal-*.whl | tail -n 1)"
  log "checking repaired runtime wheel with delocate-listdeps"
  delocate-listdeps --all "${repaired_runtime_wheel}"

  log "building main Python wheel"
  "${PY}" -m build --wheel \
    --outdir "${DIST_DIR}" \
    "${ATTEMORY_ROOT}"

  cleanup_python_build_artifacts
}

smoke_test_wheels() {
  if [[ "${SMOKE_TEST}" != "1" ]]; then
    return
  fi

  local smoke_dir="${BUILD_ROOT}/smoke"
  local main_wheel
  local runtime_wheel
  main_wheel="$(ls -1 "${DIST_DIR}"/attemory-*.whl | tail -n 1)"
  runtime_wheel="$(ls -1 "${DIST_DIR}"/attemory_runtime_macos_metal-*.whl | tail -n 1)"

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

  require_macos
  require_file "${ATTEMORY_ROOT}/pyproject.toml"

  BASE_PY="$(select_python)"
  export MACOSX_DEPLOYMENT_TARGET
  export MACOS_ARCH
  export ARCHFLAGS="-arch ${MACOS_ARCH}"

  log "attemory root: ${ATTEMORY_ROOT}"
  log "base python: ${BASE_PY}"
  log "macOS arch: ${MACOS_ARCH}"
  log "deployment target: ${MACOSX_DEPLOYMENT_TARGET}"

  if [[ "${CLEAN_BUILD_ROOT}" == "1" ]]; then
    rm -rf "${BUILD_ROOT}"
  fi
  mkdir -p "${BUILD_ROOT}" "${DIST_DIR}"

  configure_atmcore_sdk
  log "attemory-core SDK: ${ATMCORE_SDK}"

  prepare_python_build_env
  require_command cmake
  require_command otool
  require_command install_name_tool
  require_command codesign
  require_command delocate-listdeps
  require_command delocate-wheel

  build_attemory
  collect_runtime
  build_wheels
  smoke_test_wheels

  log "done"
  log "wheels: ${DIST_DIR}"
}

main "$@"
