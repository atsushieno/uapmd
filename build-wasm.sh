#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
WASM_ASSETS_DIR="${REPO_ROOT}/tools/uapmd-app/web"
WASM_STUB_DIR="${REPO_ROOT}/tools/uapmd-app"
DIST_DIR="${WASM_ASSETS_DIR}/dist"
EMSDK_INFO_FILE="${WASM_ASSETS_DIR}/.emsdk-info"
USE_SYSTEM_EMSDK=${UAPMD_USE_SYSTEM_EMSDK:-false}

ensure_emsdk() {
    if [[ "${USE_SYSTEM_EMSDK}" == "true" ]]; then
        if ! command -v emcc >/dev/null 2>&1; then
            echo "error: emcc not found" >&2
            exit 1
        fi
        return
    fi

    cmake -P "${REPO_ROOT}/cmake/FetchEmsdk.cmake" \
        -DUAPMD_EMSDK_INSTALL_TARGET="${UAPMD_EMSDK_VERSION:-latest}" \
        -DUAPMD_EMSDK_GIT_TAG="${UAPMD_EMSDK_GIT_TAG:-main}" \
        -DUAPMD_EMSDK_INFO_FILE="${EMSDK_INFO_FILE}" >/dev/null

    if [[ ! -f "${EMSDK_INFO_FILE}" ]]; then
        echo "error: failed to prepare emsdk" >&2
        exit 1
    fi

    # shellcheck disable=SC1090
    source "${EMSDK_INFO_FILE}"
    # shellcheck disable=SC1090
    source "${UAPMD_EMSDK_ENV_SCRIPT}"
}

ensure_emsdk

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"

echo "Compiling stub runtime..."
emcc -s WASM=1 \
     -s MODULARIZE=1 \
     -s EXPORT_NAME="createUapmd" \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s MAXIMUM_MEMORY=64MB \
     -s "EXPORTED_RUNTIME_METHODS=['cwrap','getValue','setValue','addFunction','removeFunction','UTF8ToString','stringToUTF8']" \
     -s "EXPORTED_FUNCTIONS=['_malloc','_free']" \
     -I "${WASM_STUB_DIR}" \
     "${WASM_STUB_DIR}/uapmd_wasm.cpp" \
     -o "${DIST_DIR}/uapmd.js" \
     -O3

echo "Stub built under ${DIST_DIR}" 
