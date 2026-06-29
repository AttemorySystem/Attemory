#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage:
  ./prepare_bench.sh longmemeval
  ./prepare_bench.sh locomo
  ./prepare_bench.sh semble
  ./prepare_bench.sh sweqa
  ./prepare_bench.sh longmemeval locomo semble sweqa
  ./prepare_bench.sh all

This downloads the pinned upstream benchmark repos and applies the Attemory
adapter patches. The command is safe to run again: existing checkouts are reused
and already-applied patches are skipped.
EOF
}

download_git_commit() {
    local dir_name="$1"
    local repo_url="$2"
    local commit_hash="$3"
    local target_dir="${ROOT_DIR}/${dir_name}"

    if [ ! -d "${target_dir}/.git" ]; then
        mkdir -p "${target_dir}"
        git -C "${target_dir}" init -q
        git -C "${target_dir}" remote add origin "${repo_url}"
    elif ! git -C "${target_dir}" remote get-url origin >/dev/null 2>&1; then
        git -C "${target_dir}" remote add origin "${repo_url}"
    fi

    echo "[prepare] ${dir_name}: fetching ${commit_hash}"
    git -C "${target_dir}" fetch --depth 1 origin "${commit_hash}" -q

    if git -C "${target_dir}" diff --quiet && git -C "${target_dir}" diff --cached --quiet; then
        git -C "${target_dir}" checkout -q FETCH_HEAD
    else
        echo "[prepare] ${dir_name}: checkout has local changes; keeping existing files"
    fi
}

download_file() {
    local url="$1"
    local out="$2"

    if [ -s "${out}" ]; then
        echo "[prepare] exists: ${out}"
        return
    fi

    mkdir -p "$(dirname "${out}")"
    echo "[prepare] download: ${url}"
    if command -v curl >/dev/null 2>&1; then
        curl -L "${url}" -o "${out}"
    else
        wget -O "${out}" "${url}"
    fi
}

apply_patch_once() {
    local dir_name="$1"
    local patch_file="$2"
    local target_dir="${ROOT_DIR}/${dir_name}"
    local patch_path="${ROOT_DIR}/${patch_file}"

    if git -C "${target_dir}" apply --check "${patch_path}" >/dev/null 2>&1; then
        echo "[prepare] ${dir_name}: applying ${patch_file}"
        git -C "${target_dir}" apply "${patch_path}"
    elif git -C "${target_dir}" apply -R --check "${patch_path}" >/dev/null 2>&1; then
        echo "[prepare] ${dir_name}: ${patch_file} already applied"
    else
        echo "[prepare] ${dir_name}: cannot apply ${patch_file}; inspect the checkout" >&2
        return 1
    fi
}

prepare_longmemeval() {
    download_git_commit \
        LongMemEval \
        https://github.com/xiaowu0162/LongMemEval.git \
        982fbd7045c9977e9119b5424cab0d7790d19413

    download_file \
        https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned/resolve/main/longmemeval_m_cleaned.json \
        "${ROOT_DIR}/LongMemEval/data/longmemeval_m_cleaned.json"
    download_file \
        https://huggingface.co/datasets/xiaowu0162/longmemeval-cleaned/resolve/main/longmemeval_s_cleaned.json \
        "${ROOT_DIR}/LongMemEval/data/longmemeval_s_cleaned.json"

    apply_patch_once LongMemEval lme.patch
}

prepare_semble() {
    download_git_commit \
        semble \
        https://github.com/MinishLab/semble.git \
        49bd6e216d7f35f45c4626de35763b3c6f7c9c3f

    apply_patch_once semble semble.patch
}

prepare_locomo() {
    download_git_commit \
        EverOS \
        https://github.com/EverMind-AI/EverOS.git \
        0f14d05aec5b9a9d867307a0945834c4d0c44c3e

    apply_patch_once EverOS locomo.patch
}

prepare_sweqa() {
    download_git_commit \
        SWE-QA-Bench \
        https://github.com/peng-weihan/SWE-QA-Bench.git \
        d7bf283d65f4bbdc86ead92fc130eee4986355f0

    apply_patch_once SWE-QA-Bench sweqa.patch
}

if [ "$#" -eq 0 ]; then
    usage
    exit 1
fi

for bench in "$@"; do
    case "${bench}" in
        longmemeval)
            prepare_longmemeval
            ;;
        semble)
            prepare_semble
            ;;
        locomo)
            prepare_locomo
            ;;
        sweqa)
            prepare_sweqa
            ;;
        all)
            prepare_longmemeval
            prepare_semble
            prepare_locomo
            prepare_sweqa
            ;;
        -h|--help|help)
            usage
            ;;
        *)
            echo "Unknown benchmark: ${bench}" >&2
            usage >&2
            exit 1
            ;;
    esac
done
