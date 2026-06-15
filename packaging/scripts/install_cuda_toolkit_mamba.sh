#!/usr/bin/env bash
set -euo pipefail

CUDA_VERSION="${CUDA_VERSION:-12.6}"
CUDA_CONDA_VERSION="${CUDA_CONDA_VERSION:-12.6.3}"
CUDA_TOOLKIT_PREFIX="${CUDA_TOOLKIT_PREFIX:-/opt/cuda-${CUDA_VERSION}}"
MICROMAMBA_ROOT="${MICROMAMBA_ROOT:-/opt/micromamba}"
MICROMAMBA_BIN="${MICROMAMBA_BIN:-}"
MICROMAMBA_URL="${MICROMAMBA_URL:-https://micro.mamba.pm/api/micromamba/linux-64/latest}"
MICROMAMBA_SHA256="${MICROMAMBA_SHA256:-}"
CUDA_MAMBA_PACKAGES="${CUDA_MAMBA_PACKAGES:-cuda-compiler cuda-cudart-dev cuda-cudart-static cuda-driver-dev cuda-nvrtc-dev libcublas-dev}"
CUDA_MAMBA_CHANNEL_PRIORITY="${CUDA_MAMBA_CHANNEL_PRIORITY:-}"
MICROMAMBA_PKGS_DIR="${MICROMAMBA_PKGS_DIR:-/tmp/cuda-micromamba-pkgs}"
MAMBA_EXTRACT_THREADS="${MAMBA_EXTRACT_THREADS:-1}"
MAMBA_DOWNLOAD_THREADS="${MAMBA_DOWNLOAD_THREADS:-4}"
PRINT_ENV=0

if [[ "${1:-}" == "--print-env" ]]; then
  PRINT_ENV=1
fi

log() {
  printf '[cuda-toolkit-mamba] %s\n' "$*" >&2
}

die() {
  printf '[cuda-toolkit-mamba] error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

verify_sha256() {
  local expected="$1"
  local path="$2"
  [[ -n "${expected}" ]] || return

  if command -v sha256sum >/dev/null 2>&1; then
    printf '%s  %s\n' "${expected}" "${path}" | sha256sum -c - >/dev/null
    return
  fi
  if command -v shasum >/dev/null 2>&1; then
    printf '%s  %s\n' "${expected}" "${path}" | shasum -a 256 -c - >/dev/null
    return
  fi
  die "MICROMAMBA_SHA256 is set but no SHA-256 checker was found"
}

default_cuda_mamba_channel_priority() {
  local major="${CUDA_VERSION%%.*}"
  if [[ "${major}" =~ ^[0-9]+$ && "${major}" -ge 13 ]]; then
    printf 'flexible\n'
  else
    printf 'strict\n'
  fi
}

detect_micromamba() {
  if [[ -n "${MICROMAMBA_BIN}" ]]; then
    [[ -x "${MICROMAMBA_BIN}" ]] || die "MICROMAMBA_BIN is not executable: ${MICROMAMBA_BIN}"
    printf '%s\n' "${MICROMAMBA_BIN}"
    return
  fi

  if command -v micromamba >/dev/null 2>&1; then
    command -v micromamba
    return
  fi

  require_command curl
  require_command tar

  local bin_dir="${MICROMAMBA_ROOT}/bin"
  local archive="${MICROMAMBA_ROOT}/micromamba.tar.bz2"
  mkdir -p "${bin_dir}"

  log "downloading micromamba: ${MICROMAMBA_URL}"
  curl -fsSL "${MICROMAMBA_URL}" -o "${archive}"
  verify_sha256 "${MICROMAMBA_SHA256}" "${archive}"
  tar -xjf "${archive}" -C "${MICROMAMBA_ROOT}" bin/micromamba
  chmod +x "${bin_dir}/micromamba"
  printf '%s\n' "${bin_dir}/micromamba"
}

install_cuda_toolkit() {
  MICROMAMBA_BIN="$(detect_micromamba)"

  if [[ -x "${CUDA_TOOLKIT_PREFIX}/bin/nvcc" ]]; then
    normalize_cuda_link_layout
    if cuda_static_runtime_ready; then
      log "CUDA toolkit already installed: ${CUDA_TOOLKIT_PREFIX}"
      return
    fi
    log "CUDA toolkit exists but static runtime libraries are incomplete; installing missing packages"
  fi

  if [[ -d "${CUDA_TOOLKIT_PREFIX}" && ! -d "${CUDA_TOOLKIT_PREFIX}/conda-meta" ]]; then
    die "CUDA_TOOLKIT_PREFIX exists but is not a conda environment: ${CUDA_TOOLKIT_PREFIX}"
  fi

  local root_prefix="${MICROMAMBA_ROOT}/root"
  local package_specs=()
  read -r -a package_specs <<< "${CUDA_MAMBA_PACKAGES}"
  [[ "${#package_specs[@]}" -gt 0 ]] || die "CUDA_MAMBA_PACKAGES is empty"

  local channel_priority="${CUDA_MAMBA_CHANNEL_PRIORITY:-$(default_cuda_mamba_channel_priority)}"
  local channel_args=(--override-channels)
  case "${channel_priority}" in
    strict)
      channel_args+=(--strict-channel-priority)
      ;;
    flexible)
      channel_args+=(--channel-priority flexible)
      ;;
    *)
      die "unsupported CUDA_MAMBA_CHANNEL_PRIORITY: ${channel_priority}; expected strict or flexible"
      ;;
  esac
  channel_args+=(
    -c "nvidia/label/cuda-${CUDA_CONDA_VERSION}"
    -c nvidia
    -c conda-forge
  )

  mkdir -p "${MICROMAMBA_PKGS_DIR}"
  log "installing CUDA packages into ${CUDA_TOOLKIT_PREFIX}: ${package_specs[*]}"
  log "using mamba channel priority: ${channel_priority}"
  if [[ -d "${CUDA_TOOLKIT_PREFIX}/conda-meta" ]]; then
    env \
      CONDA_PKGS_DIRS="${MICROMAMBA_PKGS_DIR}" \
      MAMBA_EXTRACT_THREADS="${MAMBA_EXTRACT_THREADS}" \
      MAMBA_DOWNLOAD_THREADS="${MAMBA_DOWNLOAD_THREADS}" \
      "${MICROMAMBA_BIN}" --no-rc install -y \
      -r "${root_prefix}" \
      -p "${CUDA_TOOLKIT_PREFIX}" \
      "${channel_args[@]}" \
      "${package_specs[@]}" >&2
  else
    env \
      CONDA_PKGS_DIRS="${MICROMAMBA_PKGS_DIR}" \
      MAMBA_EXTRACT_THREADS="${MAMBA_EXTRACT_THREADS}" \
      MAMBA_DOWNLOAD_THREADS="${MAMBA_DOWNLOAD_THREADS}" \
      "${MICROMAMBA_BIN}" --no-rc create -y \
      -r "${root_prefix}" \
      -p "${CUDA_TOOLKIT_PREFIX}" \
      "${channel_args[@]}" \
      "${package_specs[@]}" >&2
  fi

  normalize_cuda_link_layout
}

cuda_static_runtime_ready() {
  [[ -f "${CUDA_TOOLKIT_PREFIX}/lib/libcudadevrt.a" || -f "${CUDA_TOOLKIT_PREFIX}/lib64/libcudadevrt.a" ]] || return 1
  [[ -f "${CUDA_TOOLKIT_PREFIX}/lib/libcudart_static.a" || -f "${CUDA_TOOLKIT_PREFIX}/lib64/libcudart_static.a" ]] || return 1
}

normalize_cuda_link_layout() {
  local lib_dir="${CUDA_TOOLKIT_PREFIX}/lib"
  local lib64_dir="${CUDA_TOOLKIT_PREFIX}/lib64"
  mkdir -p "${lib_dir}" "${lib64_dir}"

  local lib
  for lib in libcudadevrt.a libcudart_static.a; do
    if [[ -f "${lib_dir}/${lib}" || -f "${lib64_dir}/${lib}" ]]; then
      continue
    fi

    local found
    found="$(find "${CUDA_TOOLKIT_PREFIX}" -type f -name "${lib}" -print -quit)"
    [[ -n "${found}" ]] || continue
    ln -sf "${found}" "${lib_dir}/${lib}"
    ln -sf "${found}" "${lib64_dir}/${lib}"
  done
}

validate_cuda_toolkit() {
  [[ -x "${CUDA_TOOLKIT_PREFIX}/bin/nvcc" ]] || die "missing nvcc after CUDA install: ${CUDA_TOOLKIT_PREFIX}/bin/nvcc"
  cuda_static_runtime_ready || die "missing CUDA static runtime libraries after CUDA install"

  local detected
  detected="$("${CUDA_TOOLKIT_PREFIX}/bin/nvcc" --version | sed -n 's/.*release \([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' | head -n 1)"
  [[ -n "${detected}" ]] || die "unable to detect CUDA version from ${CUDA_TOOLKIT_PREFIX}/bin/nvcc"
  [[ "${detected}" == "${CUDA_VERSION}" ]] || die "CUDA ${CUDA_VERSION} is required, got ${detected}"
}

cuda_library_dirs() {
  local candidates=(
    "${CUDA_TOOLKIT_PREFIX}/lib"
    "${CUDA_TOOLKIT_PREFIX}/lib64"
    "${CUDA_TOOLKIT_PREFIX}/targets/x86_64-linux/lib"
    "${CUDA_TOOLKIT_PREFIX}/lib/stubs"
    "${CUDA_TOOLKIT_PREFIX}/lib64/stubs"
    "${CUDA_TOOLKIT_PREFIX}/targets/x86_64-linux/lib/stubs"
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

print_env() {
  local library_dirs
  library_dirs="$(cuda_library_dirs)"

  printf 'export CUDA_HOME=%q\n' "${CUDA_TOOLKIT_PREFIX}"
  printf 'export CUDA_PATH=%q\n' "${CUDA_TOOLKIT_PREFIX}"
  printf 'export CUDAToolkit_ROOT=%q\n' "${CUDA_TOOLKIT_PREFIX}"
  printf 'export CUDACXX=%q\n' "${CUDA_TOOLKIT_PREFIX}/bin/nvcc"
  printf 'export CUDA_LIBRARY_DIRS=%q\n' "${library_dirs}"
  printf 'export PATH=%q\n' "${CUDA_TOOLKIT_PREFIX}/bin:${PATH}"
}

main() {
  [[ "$(uname -s)" == "Linux" ]] || die "CUDA toolkit install currently supports Linux only"
  install_cuda_toolkit
  validate_cuda_toolkit
  if [[ "${PRINT_ENV}" == "1" ]]; then
    print_env
  else
    log "CUDA toolkit ready: ${CUDA_TOOLKIT_PREFIX}"
  fi
}

main "$@"
