#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ATTEMORY_ROOT="${ATTEMORY_ROOT:-$(cd -- "${SCRIPT_DIR}/../.." && pwd)}"
DIST_DIR="${DIST_DIR:-${ATTEMORY_ROOT}/dist}"
TEST_ROOT="${TEST_ROOT:-${ATTEMORY_ROOT}/package_testing}"
LOG_DIR="${LOG_DIR:-${TEST_ROOT}/logs}"
HOST="${HOST:-127.0.0.1}"
SERVER_START_TIMEOUT="${SERVER_START_TIMEOUT:-3600}"
MODEL_TIER="${MODEL_TIER:-tiny}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
CUDA_WHEEL_INDEX_ROOT="${CUDA_WHEEL_INDEX_ROOT:-https://attemorysystem.github.io/Attemory/whl}"

usage() {
  cat <<'EOF'
Usage: test_linux_runtime_wheels.sh local|pip

Modes:
  local  Install attemory and Linux runtime wheels from ./dist.
  pip    Install each documented Linux runtime extra with pip. CUDA extras use
         the GitHub Pages wheel index from CUDA_WHEEL_INDEX_ROOT.

Environment:
  ATTEMORY_ROOT          Repository root; defaults to the script's repo root.
  DIST_DIR               Wheel directory for local mode; defaults to $ATTEMORY_ROOT/dist.
  TEST_ROOT              Test working directory; defaults to $ATTEMORY_ROOT/package_testing.
  SERVER_START_TIMEOUT   Seconds to wait for server health; defaults to 3600.
  MODEL_TIER             Server model tier; defaults to tiny.
  PIP_RUNTIME_EXTRAS     Space-separated extras for pip mode; defaults to doc/usage.md Linux extras.
  CUDA_WHEEL_INDEX_ROOT   GitHub Pages wheel index root for CUDA pip tests.
                          Defaults to https://attemorysystem.github.io/Attemory/whl.
EOF
}

log() {
  printf '[package-test] %s\n' "$*"
}

die() {
  printf '[package-test] error: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

safe_name() {
  local name="$1"
  name="${name%.whl}"
  printf '%s' "${name}" | tr -c 'A-Za-z0-9._-' '_'
}

select_attemory_wheel() {
  find -L "${DIST_DIR}" -maxdepth 1 -type f \
    -name 'attemory-*.whl' \
    ! -name 'attemory_runtime_*' \
    | sort \
    | tail -n 1
}

select_linux_runtime_wheels() {
  find -L "${DIST_DIR}" -maxdepth 1 -type f \
    -name 'attemory_runtime_linux_*.whl' \
    | sort
}

select_pip_runtime_extras() {
  if [[ -n "${PIP_RUNTIME_EXTRAS:-}" ]]; then
    local extra
    for extra in ${PIP_RUNTIME_EXTRAS}; do
      printf '%s\n' "${extra}"
    done
    return
  fi

  printf '%s\n' \
    cpu \
    cuda \
    cuda-cu121 \
    cuda-cu124 \
    cuda-cu126 \
    cuda-cu129
}

select_examples() {
  find "${ATTEMORY_ROOT}/examples" -maxdepth 1 -type f \
    -name '*.py' \
    | sort
}

runtime_backend() {
  local runtime_name="$1"
  if [[ "${runtime_name}" == *cuda* ]]; then
    printf 'gpu\n'
  else
    printf 'cpu\n'
  fi
}

cuda_index_tag_for_extra() {
  local extra="$1"
  case "${extra}" in
    cuda|gpu|cuda-cu126|linux-cuda-cu126|linux-gpu-cu126)
      printf 'cu126\n'
      ;;
    cuda-cu121|linux-cuda-cu121|linux-gpu-cu121)
      printf 'cu121\n'
      ;;
    cuda-cu124|linux-cuda-cu124|linux-gpu-cu124)
      printf 'cu124\n'
      ;;
    cuda-cu129|linux-cuda-cu129|linux-gpu-cu129)
      printf 'cu129\n'
      ;;
    *)
      return 1
      ;;
  esac
}

pip_extra_index_url_for_extra() {
  local extra="$1"
  local cuda_tag
  if ! cuda_tag="$(cuda_index_tag_for_extra "${extra}")"; then
    return 1
  fi
  printf '%s/%s/\n' "${CUDA_WHEEL_INDEX_ROOT%/}" "${cuda_tag}"
}

find_free_port() {
  "${PYTHON_BIN}" - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
}

wait_for_server() {
  local python_bin="$1"
  local pid="$2"
  local port="$3"
  local server_log="$4"
  local start="${SECONDS}"
  local deadline=$((SECONDS + SERVER_START_TIMEOUT))
  local next_log=$((SECONDS + 30))

  while (( SECONDS < deadline )); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      return 1
    fi
    if "${python_bin}" - "${HOST}" "${port}" >/dev/null 2>&1 <<'PY'
import sys
from attemory import AttemoryClient

host, port = sys.argv[1], sys.argv[2]
client = AttemoryClient(host=host, port=int(port), timeout=2.0)
if not client.health():
    raise SystemExit(1)
PY
    then
      return 0
    fi
    if (( SECONDS >= next_log )); then
      log "still waiting for server pid=${pid} port=${port} elapsed=$((SECONDS - start))s"
      if [[ -s "${server_log}" ]]; then
        tail -n 20 "${server_log}" || true
      fi
      next_log=$((SECONDS + 30))
    fi
    sleep 1
  done

  return 1
}

cleanup_run() {
  if [[ -n "${CURRENT_SERVER_PID:-}" ]] && kill -0 "${CURRENT_SERVER_PID}" 2>/dev/null; then
    kill "${CURRENT_SERVER_PID}" 2>/dev/null || true
    wait "${CURRENT_SERVER_PID}" 2>/dev/null || true
  fi
  if declare -F deactivate >/dev/null 2>&1; then
    deactivate || true
  fi
  if [[ -n "${CURRENT_VENV_DIR:-}" ]]; then
    rm -rf "${CURRENT_VENV_DIR}"
  fi
  if [[ -n "${CURRENT_WORK_DIR:-}" ]]; then
    rm -rf "${CURRENT_WORK_DIR}"
  fi
}

run_one_runtime() (
  local install_mode="$1"
  local runtime_ref="$2"
  local attemory_wheel="${3:-}"
  local runtime_label
  local runtime_install_spec
  local test_name
  local backend
  local port
  local server_log
  local extra_index_url
  local pip_args

  if [[ "${install_mode}" == "local" ]]; then
    runtime_label="$(basename "${runtime_ref}")"
    runtime_install_spec="${runtime_ref}"
  else
    runtime_label="attemory[${runtime_ref}]"
    runtime_install_spec="${runtime_label}"
  fi

  test_name="$(safe_name "${install_mode}-${runtime_label}")"
  backend="$(runtime_backend "${runtime_label}")"
  port="$(find_free_port)"
  CURRENT_SERVER_PID=""
  CURRENT_VENV_DIR="${TEST_ROOT}/${test_name}-venv"
  CURRENT_WORK_DIR="${TEST_ROOT}/${test_name}-work"
  server_log="${LOG_DIR}/${test_name}.server.log"
  trap cleanup_run EXIT

  rm -rf "${CURRENT_VENV_DIR}" "${CURRENT_WORK_DIR}" || exit 1
  mkdir -p "${CURRENT_WORK_DIR}/data" "${CURRENT_WORK_DIR}/cache" || exit 1

  log "install mode: ${install_mode}"
  if [[ "${install_mode}" == "local" ]]; then
    log "runtime wheel: ${runtime_ref}"
    log "main wheel: ${attemory_wheel}"
  else
    log "pip install spec: ${runtime_install_spec}"
  fi
  log "backend: ${backend}"
  log "server log: ${server_log}"

  "${PYTHON_BIN}" -m venv "${CURRENT_VENV_DIR}" || exit 1
  # shellcheck disable=SC1091
  source "${CURRENT_VENV_DIR}/bin/activate" || exit 1
  unset PYTHONPATH

  python -m pip install -U pip || exit 1
  if [[ "${install_mode}" == "local" ]]; then
    python -m pip install "${attemory_wheel}" "${runtime_ref}" || exit 1
  else
    extra_index_url=""
    pip_args=("${runtime_install_spec}")
    if extra_index_url="$(pip_extra_index_url_for_extra "${runtime_ref}")"; then
      log "CUDA wheel index: ${extra_index_url}"
      pip_args+=(--extra-index-url "${extra_index_url}")
    fi
    python -m pip install "${pip_args[@]}" || exit 1
  fi
  python - <<'PY' || exit 1
from attemory.runtime import resolve_runtime

print(f"runtime server binary: {resolve_runtime().server_binary}")
PY

  attemory-server \
    "--${MODEL_TIER}" \
    --backend "${backend}" \
    --host "${HOST}" \
    --port "${port}" \
    --data-dir "${CURRENT_WORK_DIR}/data" \
    --cache-dir "${CURRENT_WORK_DIR}/cache" \
    >"${server_log}" 2>&1 &
  CURRENT_SERVER_PID="$!"

  log "waiting for server pid=${CURRENT_SERVER_PID} port=${port}"
  if ! wait_for_server "${CURRENT_VENV_DIR}/bin/python" "${CURRENT_SERVER_PID}" "${port}" "${server_log}"; then
    log "server failed to become healthy; tail of server log:"
    tail -n 80 "${server_log}" || true
    exit 1
  fi

  local example
  while IFS= read -r example; do
    local example_name
    example_name="$(basename "${example}" .py)"
    log "running example: ${example}"
    "${CURRENT_VENV_DIR}/bin/python" "${example}" \
      --host "${HOST}" \
      --port "${port}" \
      --session-id "package-test-${test_name}-${example_name}" || exit 1
  done < <(select_examples)

  log "passed: ${runtime_label}"
)

main() {
  local install_mode="${1:-}"
  if [[ "$#" -ne 1 || "${install_mode}" == "-h" || "${install_mode}" == "--help" ]]; then
    usage
    if [[ "$#" -eq 1 && ( "${install_mode}" == "-h" || "${install_mode}" == "--help" ) ]]; then
      exit 0
    fi
    exit 2
  fi
  case "${install_mode}" in
    local|pip) ;;
    *)
      usage >&2
      die "invalid mode: ${install_mode}; expected local or pip"
      ;;
  esac

  require_command "${PYTHON_BIN}"
  [[ -d "${ATTEMORY_ROOT}/examples" ]] || die "missing examples directory: ${ATTEMORY_ROOT}/examples"

  mkdir -p "${TEST_ROOT}" "${LOG_DIR}"

  local examples=()
  mapfile -t examples < <(select_examples)
  [[ "${#examples[@]}" -gt 0 ]] || die "no Python examples found in ${ATTEMORY_ROOT}/examples"

  local failures=0

  if [[ "${install_mode}" == "pip" ]]; then
    local pip_runtime_extras=()
    mapfile -t pip_runtime_extras < <(select_pip_runtime_extras)
    [[ "${#pip_runtime_extras[@]}" -gt 0 ]] || die "no pip runtime extras selected"

    local runtime_extra
    for runtime_extra in "${pip_runtime_extras[@]}"; do
      local test_name
      local log_file
      test_name="$(safe_name "${install_mode}-attemory[${runtime_extra}]")"
      log_file="${LOG_DIR}/${test_name}.log"
      log "testing attemory[${runtime_extra}]"
      if run_one_runtime "${install_mode}" "${runtime_extra}" >"${log_file}" 2>&1; then
        log "passed attemory[${runtime_extra}] log=${log_file}"
      else
        log "failed attemory[${runtime_extra}] log=${log_file}"
        failures=$((failures + 1))
      fi
    done
  else
    [[ -d "${DIST_DIR}" ]] || die "missing dist directory: ${DIST_DIR}"

    local attemory_wheel
    attemory_wheel="$(select_attemory_wheel)"
    [[ -n "${attemory_wheel}" ]] || die "no attemory main wheel found in ${DIST_DIR}"

    local runtime_wheels=()
    mapfile -t runtime_wheels < <(select_linux_runtime_wheels)
    [[ "${#runtime_wheels[@]}" -gt 0 ]] || die "no Linux runtime wheels found in ${DIST_DIR}"

    local runtime_wheel
    for runtime_wheel in "${runtime_wheels[@]}"; do
      local test_name
      local log_file
      test_name="$(safe_name "${install_mode}-$(basename "${runtime_wheel}")")"
      log_file="${LOG_DIR}/${test_name}.log"
      log "testing $(basename "${runtime_wheel}")"
      if run_one_runtime "${install_mode}" "${runtime_wheel}" "${attemory_wheel}" >"${log_file}" 2>&1; then
        log "passed $(basename "${runtime_wheel}") log=${log_file}"
      else
        log "failed $(basename "${runtime_wheel}") log=${log_file}"
        failures=$((failures + 1))
      fi
    done
  fi

  if [[ "${failures}" -ne 0 ]]; then
    die "${failures} ${install_mode} runtime test(s) failed; see ${LOG_DIR}"
  fi

  log "all ${install_mode} Linux runtime tests passed; logs: ${LOG_DIR}"
}

main "$@"
