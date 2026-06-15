#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ATTEMORY_ROOT="${ATTEMORY_ROOT:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}"
ATMCORE_SDK="${ATMCORE_SDK:-}"

CUDA_VERSION="${CUDA_VERSION:-}"
CUDA_TAG="${CUDA_TAG:-}"
POSITIONAL_ARGS=()

log() {
  printf '[%s-wheel] %s\n' "${CUDA_VARIANT:-linux-cuda}" "$*"
}

die() {
  printf '[%s-wheel] error: %s\n' "${CUDA_VARIANT:-linux-cuda}" "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage: build_linux_cuda_wheel.sh --cuda cu126

Options:
  --cuda TAG|VERSION       CUDA release to build, e.g. cu126 or 12.6
  --cuda-tag TAG           CUDA tag to build, e.g. cu126
  --cuda-version VERSION   CUDA version to build, e.g. 12.6
  -h, --help               Show this help

Environment overrides remain supported, including CUDA_TAG, CUDA_VERSION,
CUDA_CONDA_VERSION, CUDA_TOOLKIT_PREFIX, ATMCORE_SDK, and DIST_DIR.
EOF
}

cuda_version_for_tag() {
  case "$1" in
    cu121) printf '12.1\n' ;;
    cu124) printf '12.4\n' ;;
    cu126) printf '12.6\n' ;;
    cu129) printf '12.9\n' ;;
    cu132) printf '13.2\n' ;;
    *) die "unsupported CUDA tag: $1" ;;
  esac
}

cuda_tag_for_version() {
  case "$1" in
    12.1) printf 'cu121\n' ;;
    12.4) printf 'cu124\n' ;;
    12.6) printf 'cu126\n' ;;
    12.9) printf 'cu129\n' ;;
    13.2) printf 'cu132\n' ;;
    *) die "unsupported CUDA version: $1" ;;
  esac
}

set_cuda_release() {
  local value="${1#linux-cuda-}"
  if [[ "${value}" == cu[0-9][0-9][0-9] ]]; then
    CUDA_TAG="${value}"
    CUDA_VERSION="$(cuda_version_for_tag "${CUDA_TAG}")"
    return
  fi
  if [[ "${value}" =~ ^[0-9]+[.][0-9]+$ ]]; then
    CUDA_VERSION="${value}"
    CUDA_TAG="$(cuda_tag_for_version "${CUDA_VERSION}")"
    return
  fi
  die "invalid CUDA release: $1; expected cu126 or 12.6"
}

parse_args() {
  while [[ "$#" -gt 0 ]]; do
    case "$1" in
      --cuda|--cuda-tag|--cuda-version)
        [[ "$#" -ge 2 ]] || die "$1 requires a value"
        set_cuda_release "$2"
        shift 2
        ;;
      --cuda=*|--cuda-tag=*|--cuda-version=*)
        set_cuda_release "${1#*=}"
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      --)
        shift
        POSITIONAL_ARGS+=("$@")
        break
        ;;
      *)
        die "unknown argument: $1"
        ;;
    esac
  done
}

parse_args "$@"
if [[ "${#POSITIONAL_ARGS[@]}" -gt 0 ]]; then
  set -- "${POSITIONAL_ARGS[@]}"
else
  set --
fi
if [[ -n "${CUDA_TAG}" && -z "${CUDA_VERSION}" ]]; then
  CUDA_VERSION="$(cuda_version_for_tag "${CUDA_TAG}")"
elif [[ -n "${CUDA_VERSION}" && -z "${CUDA_TAG}" ]]; then
  CUDA_TAG="$(cuda_tag_for_version "${CUDA_VERSION}")"
elif [[ -z "${CUDA_VERSION}" && -z "${CUDA_TAG}" ]]; then
  set_cuda_release cu126
fi

CUDA_VARIANT="${CUDA_VARIANT:-linux-cuda-${CUDA_TAG}}"
CUDA_PACKAGE_DIR="${CUDA_PACKAGE_DIR:-${ATTEMORY_ROOT}/packaging/runtime-linux-cuda-${CUDA_TAG}}"
CUDA_MODULE_NAME="${CUDA_MODULE_NAME:-attemory_runtime_linux_cuda_${CUDA_TAG}}"

BUILD_ROOT="${BUILD_ROOT:-/tmp/attemory-manylinux2014-${CUDA_VARIANT}}"
ATTEMORY_BUILD_DIR="${ATTEMORY_BUILD_DIR:-${BUILD_ROOT}/attemory}"
DIST_DIR="${DIST_DIR:-${ATTEMORY_ROOT}/dist}"
RAW_DIST_DIR="${RAW_DIST_DIR:-${DIST_DIR}/raw-${CUDA_VARIANT}}"

MANYLINUX_PLAT_EXPLICIT="${MANYLINUX_PLAT+x}"
MANYLINUX_PLAT="${MANYLINUX_PLAT:-manylinux2014_x86_64}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"
PYTHON_BIN="${PYTHON_BIN:-}"
PYTHON_TAG="${PYTHON_TAG:-cp310-cp310}"
INSTALL_PYTHON_BUILD_DEPS="${INSTALL_PYTHON_BUILD_DEPS:-1}"
SMOKE_TEST="${SMOKE_TEST:-1}"
CUDA_SMOKE_HELP="${CUDA_SMOKE_HELP:-0}"
CLEAN_BUILD_ROOT="${CLEAN_BUILD_ROOT:-1}"
CLEAN_DIST="${CLEAN_DIST:-1}"
STRICT_MANYLINUX_BASELINE="${STRICT_MANYLINUX_BASELINE:-1}"
ATTEMORY_CMAKE_ARGS="${ATTEMORY_CMAKE_ARGS:-}"
HOST_UID="${HOST_UID:-}"
HOST_GID="${HOST_GID:-}"
CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
CUDA_ARCH_PROFILE="${CUDA_ARCH_PROFILE:-compat}"
CUDA_ARCHITECTURES="${CUDA_ARCHITECTURES:-}"
CUDA_LIBRARY_DIRS="${CUDA_LIBRARY_DIRS:-}"
CUDA_CONDA_VERSION="${CUDA_CONDA_VERSION:-}"
CUDA_TOOLKIT_PREFIX="${CUDA_TOOLKIT_PREFIX:-/opt/cuda-${CUDA_VERSION}}"
INSTALL_CUDA_TOOLKIT_WITH_MAMBA="${INSTALL_CUDA_TOOLKIT_WITH_MAMBA:-0}"
CUDA_MAMBA_PACKAGES="${CUDA_MAMBA_PACKAGES:-cuda-compiler cuda-cudart-dev cuda-cudart-static cuda-driver-dev cuda-nvrtc-dev libcublas-dev}"
CUDA_MAMBA_CHANNEL_PRIORITY="${CUDA_MAMBA_CHANNEL_PRIORITY:-}"
MICROMAMBA_PKGS_DIR="${MICROMAMBA_PKGS_DIR:-/tmp/cuda-micromamba-pkgs}"
MICROMAMBA_URL="${MICROMAMBA_URL:-}"
MICROMAMBA_SHA256="${MICROMAMBA_SHA256:-}"
MAMBA_EXTRACT_THREADS="${MAMBA_EXTRACT_THREADS:-1}"
MAMBA_DOWNLOAD_THREADS="${MAMBA_DOWNLOAD_THREADS:-4}"
STRIP_RUNTIME_BINARIES="${STRIP_RUNTIME_BINARIES:-1}"
STRIP_BIN="${STRIP_BIN:-strip}"
atmcore_runtime_dirs=""

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

default_cuda_conda_version() {
  case "${CUDA_VERSION}" in
    12.1)
      printf '12.1.1\n'
      ;;
    12.4)
      printf '12.4.1\n'
      ;;
    12.6)
      printf '12.6.3\n'
      ;;
    12.9)
      printf '12.9.1\n'
      ;;
    13.2)
      printf '13.2.1\n'
      ;;
    *)
      die "unsupported CUDA_VERSION for packaged CUDA wheels: ${CUDA_VERSION}"
      ;;
  esac
}

configure_cuda_release() {
  local expected_tag="cu${CUDA_VERSION//./}"
  local expected_variant="linux-cuda-${CUDA_TAG}"
  [[ "${CUDA_TAG}" == "${expected_tag}" ]] || die "CUDA_TAG=${CUDA_TAG} does not match CUDA_VERSION=${CUDA_VERSION}; expected ${expected_tag}"
  [[ "${CUDA_VARIANT}" == "${expected_variant}" ]] || die "CUDA_VARIANT=${CUDA_VARIANT} does not match CUDA_TAG=${CUDA_TAG}; expected ${expected_variant}"
  [[ -d "${CUDA_PACKAGE_DIR}" ]] || die "missing runtime package directory: ${CUDA_PACKAGE_DIR}"
  if [[ -z "${CUDA_CONDA_VERSION}" ]]; then
    CUDA_CONDA_VERSION="$(default_cuda_conda_version)"
  fi
  if [[ -z "${MANYLINUX_PLAT_EXPLICIT}" && "${CUDA_TAG}" == "cu129" ]]; then
    MANYLINUX_PLAT="manylinux_2_28_x86_64"
  fi
}

normalize_cuda_architectures() {
  if [[ -z "${CUDA_ARCHITECTURES}" ]]; then
    CUDA_ARCHITECTURES="$(cuda_architectures_for_profile "${CUDA_ARCH_PROFILE}")"
    log "using CUDA_ARCH_PROFILE=${CUDA_ARCH_PROFILE}: ${CUDA_ARCHITECTURES}"
  fi
  [[ -n "${CUDA_ARCHITECTURES}" ]] || return
  if [[ "${CUDA_ARCHITECTURES}" == *"-real"* || "${CUDA_ARCHITECTURES}" == *"-virtual"* || "${CUDA_ARCHITECTURES}" == "native" ]]; then
    return
  fi

  local input="${CUDA_ARCHITECTURES}"
  local IFS=';'
  local archs=()
  read -r -a archs <<< "${input}"
  [[ "${#archs[@]}" -gt 0 ]] || return

  local normalized=()
  local arch
  for arch in "${archs[@]}"; do
    [[ -n "${arch}" ]] || continue
    normalized+=("${arch}-real")
  done
  [[ "${#normalized[@]}" -gt 0 ]] || return
  normalized+=("${archs[$((${#archs[@]} - 1))]}-virtual")

  local old_ifs="${IFS}"
  IFS=';'
  CUDA_ARCHITECTURES="${normalized[*]}"
  IFS="${old_ifs}"
  log "normalized bare CUDA_ARCHITECTURES=${input} to ${CUDA_ARCHITECTURES}"
}

cuda_architectures_for_profile() {
  local cuda_major="${CUDA_VERSION%%.*}"
  case "$1" in
    compat)
      if [[ "${cuda_major}" -ge 13 ]]; then
        printf '75-real;80-real;86-real;89-real;90-real;100-real;120-real;120-virtual\n'
      elif [[ "${CUDA_VERSION}" == "12.9" ]]; then
        printf '75-real;80-real;86-real;89-real;90-real;120a-real;90-virtual\n'
      else
        printf '70-real;75-real;80-real;86-real;89-real;90-real;90-virtual\n'
      fi
      ;;
    modern)
      if [[ "${cuda_major}" -ge 13 ]]; then
        printf '80-real;86-real;89-real;90-real;100-real;120-real;120-virtual\n'
      elif [[ "${CUDA_VERSION}" == "12.9" ]]; then
        printf '80-real;86-real;89-real;90-real;120a-real;90-virtual\n'
      else
        printf '80-real;86-real;89-real;90-real;90-virtual\n'
      fi
      ;;
    ampere)
      printf '80-real;86-real;90-virtual\n'
      ;;
    ada)
      printf '89-real;90-virtual\n'
      ;;
    hopper)
      printf '90-real;90-virtual\n'
      ;;
    *)
      die "unsupported CUDA_ARCH_PROFILE: $1"
      ;;
  esac
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
    "${CUDA_PACKAGE_DIR}/.pybuild" \
    "${CUDA_PACKAGE_DIR}/python/${CUDA_MODULE_NAME}.egg-info"
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
    "${CUDA_PACKAGE_DIR}/.pybuild" \
    "${CUDA_PACKAGE_DIR}/python/${CUDA_MODULE_NAME}.egg-info" \
    "${CUDA_PACKAGE_DIR}/python/${CUDA_MODULE_NAME}"; do
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

require_cuda_toolkit() {
  [[ -x "${CUDA_HOME}/bin/nvcc" ]] || die "missing CUDA nvcc: ${CUDA_HOME}/bin/nvcc"
  local detected
  detected="$("${CUDA_HOME}/bin/nvcc" --version | sed -n 's/.*release \([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -n 1)"
  [[ -n "${detected}" ]] || die "unable to detect CUDA version from ${CUDA_HOME}/bin/nvcc"
  [[ "${detected}" == "${CUDA_VERSION}" ]] || die "CUDA ${CUDA_VERSION} is required, got ${detected}"
}

install_cuda_toolkit_with_mamba() {
  if [[ "${INSTALL_CUDA_TOOLKIT_WITH_MAMBA}" != "1" ]]; then
    return
  fi

  log "installing CUDA ${CUDA_CONDA_VERSION} build packages with micromamba: ${CUDA_MAMBA_PACKAGES}"
  local cuda_env
  if ! cuda_env="$(
    CUDA_VERSION="${CUDA_VERSION}" \
    CUDA_CONDA_VERSION="${CUDA_CONDA_VERSION}" \
    CUDA_TOOLKIT_PREFIX="${CUDA_TOOLKIT_PREFIX}" \
    CUDA_MAMBA_PACKAGES="${CUDA_MAMBA_PACKAGES}" \
    CUDA_MAMBA_CHANNEL_PRIORITY="${CUDA_MAMBA_CHANNEL_PRIORITY}" \
    MICROMAMBA_PKGS_DIR="${MICROMAMBA_PKGS_DIR}" \
    MICROMAMBA_URL="${MICROMAMBA_URL}" \
    MICROMAMBA_SHA256="${MICROMAMBA_SHA256}" \
    MAMBA_EXTRACT_THREADS="${MAMBA_EXTRACT_THREADS}" \
    MAMBA_DOWNLOAD_THREADS="${MAMBA_DOWNLOAD_THREADS}" \
    "${SCRIPT_DIR}/install_cuda_toolkit_mamba.sh" --print-env
  )"; then
    die "failed to install CUDA ${CUDA_CONDA_VERSION} build packages"
  fi
  eval "${cuda_env}"
}

default_cuda_library_dirs() {
  local candidates=(
    "${CUDA_HOME}/lib"
    "${CUDA_HOME}/lib64"
    "${CUDA_HOME}/targets/x86_64-linux/lib"
    "${CUDA_HOME}/lib/stubs"
    "${CUDA_HOME}/lib64/stubs"
    "${CUDA_HOME}/targets/x86_64-linux/lib/stubs"
  )
  local existing=()
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -d "${candidate}" ]]; then
      existing+=("${candidate}")
    fi
  done
  local IFS=';'
  printf '%s\n' "${existing[*]}"
}

configure_cuda_library_dirs() {
  if [[ -z "${CUDA_LIBRARY_DIRS}" ]]; then
    CUDA_LIBRARY_DIRS="$(default_cuda_library_dirs)"
  fi
  [[ -n "${CUDA_LIBRARY_DIRS}" ]] || die "missing CUDA library directories under ${CUDA_HOME}"
  add_cuda_driver_stub_link_dir
}

add_cuda_driver_stub_link_dir() {
  local link_dir="${BUILD_ROOT}/cuda-driver-stubs"
  local added=0
  local runtime_dir
  local IFS=';'
  for runtime_dir in ${CUDA_LIBRARY_DIRS}; do
    if [[ -f "${runtime_dir}/libcuda.so" ]]; then
      mkdir -p "${link_dir}"
      ln -sf "${runtime_dir}/libcuda.so" "${link_dir}/libcuda.so"
      ln -sf "${runtime_dir}/libcuda.so" "${link_dir}/libcuda.so.1"
      added=1
    fi
    if [[ -f "${runtime_dir}/libnvidia-ml.so" ]]; then
      mkdir -p "${link_dir}"
      ln -sf "${runtime_dir}/libnvidia-ml.so" "${link_dir}/libnvidia-ml.so"
      ln -sf "${runtime_dir}/libnvidia-ml.so" "${link_dir}/libnvidia-ml.so.1"
      added=1
    fi
  done
  if [[ "${added}" == "1" ]]; then
    CUDA_LIBRARY_DIRS="${link_dir};${CUDA_LIBRARY_DIRS}"
  fi
}

append_runtime_dirs() {
  local base="$1"
  if [[ -n "${CUDA_LIBRARY_DIRS}" ]]; then
    printf '%s;%s\n' "${base}" "${CUDA_LIBRARY_DIRS}"
  else
    printf '%s\n' "${base}"
  fi
}

configure_atmcore_sdk() {
  configure_cuda_library_dirs

  [[ -n "${ATMCORE_SDK}" ]] || die "set ATMCORE_SDK=/path/to/attemory-core-sdk"
  ATMCORE_SDK="$(resolve_dir "${ATMCORE_SDK}")"
  require_file "${ATMCORE_SDK}/include/attemory-core/attemory-core.h"
  validate_atmcore_sdk_variant
  [[ -d "${ATMCORE_SDK}/lib" ]] || die "missing attemory-core SDK lib directory: ${ATMCORE_SDK}/lib"
  atmcore_runtime_dirs="$(append_runtime_dirs "${ATMCORE_SDK}/lib;${ATMCORE_SDK}/bin")"
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
  [[ "${sdk_variant}" == "${CUDA_VARIANT}" ]] || die "attemory-core SDK variant ${sdk_variant} does not match requested ${CUDA_VARIANT}"
}

build_attemory() {
  log "configuring attemory: ${ATTEMORY_BUILD_DIR}"
  local cmake_args=(
    -S "${ATTEMORY_ROOT}" -B "${ATTEMORY_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DATMCORE_SDK="${ATMCORE_SDK}" \
    -DATTEMORY_ALLOW_SDK_SHLIB_UNDEFINED=ON
  )
  if [[ -n "${CC:-}" ]]; then
    cmake_args+=(-DCMAKE_C_COMPILER="${CC}")
  fi
  if [[ -n "${CXX:-}" ]]; then
    cmake_args+=(-DCMAKE_CXX_COMPILER="${CXX}")
  fi
  if [[ -n "${CUDA_LIBRARY_DIRS}" ]]; then
    cmake_args+=(-DATTEMORY_EXTRA_RUNTIME_DIRS="${CUDA_LIBRARY_DIRS}")
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
  CUDA_HOME="${CUDA_HOME}" "${PY}" -B "${ATTEMORY_ROOT}/packaging/collect_runtime.py" \
    --variant "${CUDA_VARIANT}" \
    --server-binary "${ATTEMORY_BUILD_DIR}/bin/attemory_server" \
    "${runtime_dir_args[@]}" \
    --clean \
    --patch-rpath always
  strip_runtime_binaries
}

strip_runtime_binaries() {
  if [[ "${STRIP_RUNTIME_BINARIES}" != "1" ]]; then
    return
  fi
  if ! command -v "${STRIP_BIN}" >/dev/null 2>&1; then
    log "strip tool not found: ${STRIP_BIN}; skipping runtime binary strip"
    return
  fi

  local runtime_root="${CUDA_PACKAGE_DIR}/python/${CUDA_MODULE_NAME}"
  [[ -d "${runtime_root}" ]] || return

  log "stripping Linux CUDA runtime binaries"
  find "${runtime_root}" -type f \( -name '*.so' -o -name '*.so.*' -o -path '*/bin/*' \) -print0 |
    xargs -0 -r "${STRIP_BIN}" --strip-unneeded
}

runtime_ld_library_path() {
  local runtime_lib_dir="$1"
  local paths=("${runtime_lib_dir}")
  local runtime_dir
  local IFS=';'
  for runtime_dir in ${CUDA_LIBRARY_DIRS}; do
    if [[ -d "${runtime_dir}" ]]; then
      paths+=("${runtime_dir}")
    fi
  done
  IFS=':'
  printf '%s\n' "${paths[*]}"
}

build_wheels() {
  mkdir -p "${DIST_DIR}" "${RAW_DIST_DIR}"
  if [[ "${CLEAN_DIST}" == "1" ]]; then
    rm -rf "${RAW_DIST_DIR}"
    mkdir -p "${RAW_DIST_DIR}"
    rm -f "${DIST_DIR}/${CUDA_MODULE_NAME}"-*.whl
    rm -f "${DIST_DIR}"/attemory-*.whl
  fi

  cleanup_python_build_artifacts

  log "building CUDA ${CUDA_VERSION} runtime wheel"
  "${PY}" -m build --wheel \
    --outdir "${RAW_DIST_DIR}" \
    "${CUDA_PACKAGE_DIR}"

  local runtime_wheel
  runtime_wheel="$(ls -1 "${RAW_DIST_DIR}/${CUDA_MODULE_NAME}"-*.whl | tail -n 1)"

  log "checking runtime wheel with auditwheel"
  auditwheel show "${runtime_wheel}"

  log "repairing runtime wheel for ${MANYLINUX_PLAT}"
  local runtime_lib_dir="${CUDA_PACKAGE_DIR}/python/${CUDA_MODULE_NAME}/lib"
  LD_LIBRARY_PATH="$(runtime_ld_library_path "${runtime_lib_dir}")${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    auditwheel repair \
      --plat "${MANYLINUX_PLAT}" \
      --exclude libcuda.so \
      --exclude libcuda.so.1 \
      --exclude libnvidia-ml.so \
      --exclude libnvidia-ml.so.1 \
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
    local message="builder glibc is newer than ${MANYLINUX_PLAT}; use quay.io/pypa/manylinux2014_x86_64 and install CUDA with micromamba"
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

  repaired_runtime_wheel="$(ls -1 "${DIST_DIR}/${CUDA_MODULE_NAME}"-*manylinux*.whl | tail -n 1)"
  raw_runtime_wheel="$(ls -1 "${RAW_DIST_DIR}/${CUDA_MODULE_NAME}"-*.whl | tail -n 1)"

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
  "${smoke_dir}/bin/python" - <<PY
from attemory.runtime import resolve_runtime

runtime = resolve_runtime()
expected = "${CUDA_VARIANT}"
if runtime.name != expected:
    raise SystemExit(f"expected runtime {expected}, got {runtime.name}")
if not runtime.server_binary.exists():
    raise SystemExit(f"missing server binary: {runtime.server_binary}")
if runtime.lib_dir is None or not runtime.lib_dir.exists():
    raise SystemExit(f"missing runtime lib dir: {runtime.lib_dir}")
print(runtime.server_binary)
PY
  if [[ "${CUDA_SMOKE_HELP}" == "1" ]]; then
    "${smoke_dir}/bin/attemory-server" --help >/dev/null
  else
    log "skipping native server smoke test; set CUDA_SMOKE_HELP=1 on a runner with a CUDA driver"
  fi
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

  configure_cuda_release
  install_cuda_toolkit_with_mamba
  configure_linux_toolchain
  require_cuda_toolkit
  configure_atmcore_sdk
  log "attemory root: ${ATTEMORY_ROOT}"
  log "attemory-core SDK: ${ATMCORE_SDK}"
  normalize_cuda_architectures
  log "CUDA architectures: ${CUDA_ARCHITECTURES}"
  log "CUDA wheel tag: ${CUDA_TAG}"
  log "CUDA conda version: ${CUDA_CONDA_VERSION}"
  log "CUDA home: ${CUDA_HOME}"
  log "CUDA library dirs: ${CUDA_LIBRARY_DIRS}"
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
