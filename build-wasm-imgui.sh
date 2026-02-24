#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${REPO_ROOT}/cmake-build-wasm"
WASM_ASSETS_DIR="${REPO_ROOT}/tools/uapmd-app/web"
EMSDK_INFO_FILE="${WASM_ASSETS_DIR}/.emsdk-info"
USE_SYSTEM_EMSDK=false

CPM_CACHE_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --use-system)
            USE_SYSTEM_EMSDK=true
            shift
            ;;
        --help|-h)
            cat <<HELP
Usage: ./build-wasm-imgui.sh [--use-system]

Options:
  --use-system   Skip automatic emsdk checkout and expect emcmake/emcc in PATH.
HELP
            exit 0
            ;;
        *)
            if [[ "$1" == CPM_SOURCE_CACHE=* ]]; then
                CPM_CACHE_ARG="-DCPM_SOURCE_CACHE=${1#CPM_SOURCE_CACHE=}"
                shift
                continue
            fi
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done
if [[ -z "${CPM_CACHE_ARG}" && -n "${CPM_SOURCE_CACHE:-}" ]]; then
    CPM_CACHE_ARG="-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}"
fi

write_emsdk_info() {
    cat > "${EMSDK_INFO_FILE}" <<INFO
UAPMD_EMSDK_ROOT="${UAPMD_EMSDK_ROOT}"
UAPMD_EMSDK_ENV_SCRIPT="${UAPMD_EMSDK_ENV_SCRIPT}"
INFO
}

ensure_emsdk() {
    if ${USE_SYSTEM_EMSDK}; then
        if ! command -v emcmake >/dev/null 2>&1; then
            echo "error: emcmake not found; install emsdk or drop --use-system" >&2
            exit 1
        fi
        return
    fi

    local emsdk_version="${UAPMD_EMSDK_VERSION:-latest}"
    local emsdk_tag="${UAPMD_EMSDK_GIT_TAG:-main}"
    local emsdk_dir="${WASM_ASSETS_DIR}/.emsdk"

    if [[ ! -d "${emsdk_dir}/.git" ]]; then
        echo "Cloning emsdk (${emsdk_tag})..."
        rm -rf "${emsdk_dir}"
        git clone --depth 1 --branch "${emsdk_tag}" https://github.com/emscripten-core/emsdk.git "${emsdk_dir}" >/dev/null
    else
        echo "Updating emsdk checkout (${emsdk_tag})..."
        git -C "${emsdk_dir}" fetch origin "${emsdk_tag}" >/dev/null
        git -C "${emsdk_dir}" checkout "${emsdk_tag}" >/dev/null
        git -C "${emsdk_dir}" pull --ff-only >/dev/null || true
    fi

    pushd "${emsdk_dir}" >/dev/null
    ./emsdk install "${emsdk_version}" >/dev/null
    ./emsdk activate "${emsdk_version}" >/dev/null
    UAPMD_EMSDK_ROOT="${emsdk_dir}"
    UAPMD_EMSDK_ENV_SCRIPT="${emsdk_dir}/emsdk_env.sh"
    write_emsdk_info
    # shellcheck disable=SC1090
    source "${UAPMD_EMSDK_ENV_SCRIPT}"
    popd >/dev/null
}

configure_and_build() {
    rm -rf "${BUILD_DIR}"
    echo "Configuring with emcmake..."
    emcmake cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DUAPMD_TARGET_WASM=ON \
        -DUAPMD_BUILD_TESTS=OFF \
        ${CPM_CACHE_ARG}

    echo "Building..."
    cmake --build "${BUILD_DIR}" --config Release --target uapmd-app
}

ensure_emsdk
configure_and_build

echo "✓ Build artifacts ready in ${BUILD_DIR}/tools/uapmd-app" 
echo "  - Look for uapmd-app.{html,js,wasm} created by the Emscripten linker"
