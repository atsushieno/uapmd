#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
    cat >&2 <<'HELP'
Usage: prepare-gh-pages-site.sh <source-dir> <artifacts-dir> <target-dir> [version-tag]
HELP
    exit 1
fi

SOURCE_DIR="$1"
ARTIFACTS_DIR="$2"
TARGET_DIR="$3"
VERSION_TAG="${4:-}"

ROOT_INDEX="${SOURCE_DIR}/index.html"
ROOT_STYLES="${SOURCE_DIR}/styles.css"
PLAYGROUND_INDEX="${SOURCE_DIR}/playground/index.html"
API_INDEX="${SOURCE_DIR}/api/index.html"
API_LATEST_INDEX="${SOURCE_DIR}/api/latest/index.html"

if [[ ! -f "${ROOT_INDEX}" || ! -f "${ROOT_STYLES}" ]]; then
    echo "error: missing site source files under ${SOURCE_DIR}" >&2
    exit 1
fi

if [[ ! -d "${ARTIFACTS_DIR}" ]]; then
    echo "error: artifacts directory does not exist: ${ARTIFACTS_DIR}" >&2
    exit 1
fi

ARTIFACT_APP_JS="$(find "${ARTIFACTS_DIR}" -type f -name 'uapmd-app.js' | head -n 1)"
if [[ -z "${ARTIFACT_APP_JS}" ]]; then
    echo "error: missing wasm artifact uapmd-app.js under ${ARTIFACTS_DIR}" >&2
    exit 1
fi
ARTIFACT_ROOT="$(dirname "${ARTIFACT_APP_JS}")"

mkdir -p "${TARGET_DIR}" \
    "${TARGET_DIR}/playground" \
    "${TARGET_DIR}/api"

copy_static_file() {
    local src="$1"
    local dst="$2"

    mkdir -p "$(dirname "${dst}")"
    cp "${src}" "${dst}"
}

copy_bundle_tree() {
    local dst_dir="$1"

    rm -rf "${dst_dir}"
    mkdir -p "${dst_dir}"
    cp -R "${ARTIFACT_ROOT}/." "${dst_dir}/"
}

render_api_version_page() {
    local version="$1"
    local dst="$2"

    cat > "${dst}" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <title>uapmd API references / ${version}</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="../../styles.css">
</head>
<body>
<div class="shell">
    <nav class="nav">
        <a class="brand" href="../../">uapmd web</a>
        <div class="nav-links">
            <a class="nav-link" href="../../playground/">Playground</a>
            <a class="nav-link" href="../">API references</a>
        </div>
    </nav>

    <section class="hero">
        <p class="eyebrow">API references / ${version}</p>
        <h1>Under construction</h1>
        <p class="lede">
            This placeholder reserves the versioned API reference URL for
            <code>${version}</code>.
        </p>
    </section>
    <section class="panel">
        <p class="muted">
            Once versioned API publishing is designed, the generated reference set for
            <code>${version}</code> can replace this page in place.
        </p>
    </section>
</div>
</body>
</html>
EOF
}

copy_static_file "${ROOT_INDEX}" "${TARGET_DIR}/index.html"
copy_static_file "${ROOT_STYLES}" "${TARGET_DIR}/styles.css"
copy_static_file "${PLAYGROUND_INDEX}" "${TARGET_DIR}/playground/index.html"
copy_static_file "${API_INDEX}" "${TARGET_DIR}/api/index.html"

copy_bundle_tree "${TARGET_DIR}/playground/latest"

rm -rf "${TARGET_DIR}/api/latest"
mkdir -p "${TARGET_DIR}/api/latest"
copy_static_file "${API_LATEST_INDEX}" "${TARGET_DIR}/api/latest/index.html"

if [[ -n "${VERSION_TAG}" ]]; then
    SAFE_VERSION_TAG="${VERSION_TAG//\//-}"

    copy_bundle_tree "${TARGET_DIR}/playground/${SAFE_VERSION_TAG}"

    rm -rf "${TARGET_DIR}/api/${SAFE_VERSION_TAG}"
    mkdir -p "${TARGET_DIR}/api/${SAFE_VERSION_TAG}"
    render_api_version_page "${SAFE_VERSION_TAG}" "${TARGET_DIR}/api/${SAFE_VERSION_TAG}/index.html"
fi

touch "${TARGET_DIR}/.nojekyll"
