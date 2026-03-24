#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
    cat >&2 <<'HELP'
Usage: prepare-gh-pages-site.sh <source-dir> <playground-artifacts-dir> <api-artifacts-dir> <target-dir> [version-tag]
HELP
    exit 1
fi

SOURCE_DIR="$1"
PLAYGROUND_ARTIFACTS_DIR="$2"
API_ARTIFACTS_DIR="$3"
TARGET_DIR="$4"
VERSION_TAG="${5:-}"

ROOT_INDEX="${SOURCE_DIR}/index.html"
ROOT_STYLES="${SOURCE_DIR}/styles.css"
PLAYGROUND_INDEX="${SOURCE_DIR}/playground/index.html"
API_INDEX="${SOURCE_DIR}/api/index.html"

if [[ ! -f "${ROOT_INDEX}" || ! -f "${ROOT_STYLES}" ]]; then
    echo "error: missing site source files under ${SOURCE_DIR}" >&2
    exit 1
fi

if [[ ! -d "${PLAYGROUND_ARTIFACTS_DIR}" ]]; then
    echo "error: playground artifacts directory does not exist: ${PLAYGROUND_ARTIFACTS_DIR}" >&2
    exit 1
fi

if [[ ! -d "${API_ARTIFACTS_DIR}" ]]; then
    echo "error: api artifacts directory does not exist: ${API_ARTIFACTS_DIR}" >&2
    exit 1
fi

ARTIFACT_APP_JS="$(find "${PLAYGROUND_ARTIFACTS_DIR}" -type f -name 'uapmd-app.js' | head -n 1)"
if [[ -z "${ARTIFACT_APP_JS}" ]]; then
    echo "error: missing wasm artifact uapmd-app.js under ${PLAYGROUND_ARTIFACTS_DIR}" >&2
    exit 1
fi
PLAYGROUND_ARTIFACT_ROOT="$(dirname "${ARTIFACT_APP_JS}")"

API_ARTIFACT_INDEX="$(find "${API_ARTIFACTS_DIR}" -path '*/html/index.html' | head -n 1)"
if [[ -z "${API_ARTIFACT_INDEX}" ]]; then
    API_ARTIFACT_INDEX="$(find "${API_ARTIFACTS_DIR}" -type f -name 'index.html' | head -n 1)"
fi
if [[ -z "${API_ARTIFACT_INDEX}" ]]; then
    echo "error: missing api docs index.html under ${API_ARTIFACTS_DIR}" >&2
    exit 1
fi
API_ARTIFACT_ROOT="$(dirname "${API_ARTIFACT_INDEX}")"

mkdir -p "${TARGET_DIR}" \
    "${TARGET_DIR}/playground" \
    "${TARGET_DIR}/api"

copy_static_file() {
    local src="$1"
    local dst="$2"

    mkdir -p "$(dirname "${dst}")"
    cp "${src}" "${dst}"
}

normalize_version_tag() {
    local raw="$1"

    if [[ "${raw}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)([.-].*)?$ ]]; then
        printf '%s.%s.%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"
    else
        printf '%s\n' "${raw//\//-}"
    fi
}

copy_tree() {
    local src_dir="$1"
    local dst_dir="$2"

    rm -rf "${dst_dir}"
    mkdir -p "${dst_dir}"
    cp -R "${src_dir}/." "${dst_dir}/"
}

copy_static_file "${ROOT_INDEX}" "${TARGET_DIR}/index.html"
copy_static_file "${ROOT_STYLES}" "${TARGET_DIR}/styles.css"
copy_static_file "${PLAYGROUND_INDEX}" "${TARGET_DIR}/playground/index.html"
copy_static_file "${API_INDEX}" "${TARGET_DIR}/api/index.html"

copy_tree "${PLAYGROUND_ARTIFACT_ROOT}" "${TARGET_DIR}/playground/latest"

copy_tree "${API_ARTIFACT_ROOT}" "${TARGET_DIR}/api/latest"

if [[ -n "${VERSION_TAG}" ]]; then
    NORMALIZED_VERSION_TAG="$(normalize_version_tag "${VERSION_TAG}")"

    copy_tree "${PLAYGROUND_ARTIFACT_ROOT}" "${TARGET_DIR}/playground/${NORMALIZED_VERSION_TAG}"

    copy_tree "${API_ARTIFACT_ROOT}" "${TARGET_DIR}/api/${NORMALIZED_VERSION_TAG}"
fi

touch "${TARGET_DIR}/.nojekyll"
