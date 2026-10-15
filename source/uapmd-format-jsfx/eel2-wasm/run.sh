#!/usr/bin/env bash
# Usage: run.sh bench <file.jsfx>... [-- voices blocks] | run.sh check <file.jsfx>... [-- voices blocks] | run.sh spike
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../.." && pwd)"
OUT="${REPO_ROOT}/cmake-build-eel2-wasm"
# shellcheck disable=SC1090
source "${REPO_ROOT}/source/tools/uapmd-app/web/.emsdk-info"
NODE="$(ls -d "${UAPMD_EMSDK_ROOT}"/node/*/bin/node | sort -V | tail -1)"

mode="${1:-}"
[[ $# -gt 0 ]] && shift
files=()
while [[ $# -gt 0 && "$1" != "--" ]]; do files+=("$1"); shift; done
[[ "${1:-}" == "--" ]] && shift
voices="${1:-4}"
blocks="${2:-800}"

case "${mode}" in
    bench)
        for f in "${files[@]}"; do
            echo "native-jit      | $("${OUT}/native-jit/jsfx-bench" "$f" "$voices" "$blocks" 2>&1 | grep 'avg=' || echo FAIL)"
            echo "native-portable | $("${OUT}/native-portable/jsfx-bench" "$f" "$voices" "$blocks" 2>&1 | grep 'avg=' || echo FAIL)"
            echo "wasm-portable   | $("${NODE}" "${OUT}/wasm-portable/jsfx-bench.js" "$f" "$voices" "$blocks" 2>&1 | grep 'avg=' || echo FAIL)"
        done
        ;;
    check)
        tmp="${OUT}/wasm-backend/check"
        mkdir -p "${tmp}"
        for f in "${files[@]}"; do
            ri="$("${NODE}" "${OUT}/wasm-backend/jsfx-wasm-check.js" "$f" "$voices" "$blocks" interp "${tmp}/interp.raw" 2>/dev/null | grep 'gen=' || true)"
            rw="$("${NODE}" "${OUT}/wasm-backend/jsfx-wasm-check.js" "$f" "$voices" "$blocks" wasm "${tmp}/wasm.raw" 2>"${tmp}/wasm.err" | grep 'gen=' || true)"
            if [[ -z "${ri}" || -z "${rw}" ]]; then
                echo "CRASH  $(basename "$f"): $(grep -m1 -E 'RuntimeError|Error|Aborted' "${tmp}/wasm.err" | cut -c1-120)"
                continue
            fi
            cmpres="$(python3 -I "${HERE}/bench/compare-raw.py" "${tmp}/interp.raw" "${tmp}/wasm.raw")"
            echo "${rw}  vs interp ${ri##*avg=}  ${cmpres}"
            grep '\[eel2-wasm\]' "${tmp}/wasm.err" | sort | uniq -c | head -5 || true
        done
        ;;
    spike)
        "${NODE}" "${OUT}/spike/eel2-wasm-spike.js"
        ;;
    *)
        echo "usage: $0 bench|check <file.jsfx>... [-- voices blocks] | $0 spike" >&2
        exit 1
        ;;
esac
