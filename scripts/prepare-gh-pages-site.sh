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

render_api_index_page() {
    local api_dir="$1"
    shift
    local versions=("$@")
    local versions_markup=""
    local version

    if [[ ${#versions[@]} -gt 0 ]]; then
        for version in "${versions[@]}"; do
            versions_markup="${versions_markup}            <li><a href=\"./${version}/\">${version}</a></li>
"
        done
    else
        versions_markup='            <li>No versioned API snapshots have been published yet.</li>
'
    fi

    cat > "${api_dir}/index.html" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <title>uapmd API references</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="../styles.css">
</head>
<body>
<div class="shell">
    <nav class="nav">
        <a class="brand" href="../">uapmd web</a>
        <div class="nav-links">
            <a class="nav-link" href="../playground/">Playground</a>
            <a class="nav-link" href="./">API references</a>
        </div>
    </nav>

    <section class="hero">
        <p class="eyebrow">API references</p>
        <h1>Doxygen reference sets</h1>
        <p class="lede">
            The latest API docs are always refreshed on a manual Pages run. Versioned
            snapshots are retained on this site and can be republished individually.
        </p>
        <div class="actions">
            <a class="button button-primary" href="./latest/">Open latest docs</a>
        </div>
    </section>

    <div class="grid two">
        <section class="card">
            <p class="eyebrow">Current</p>
            <h2><a href="./latest/">latest</a></h2>
            <p>
                Stable entry point for the current Doxygen-generated API reference set.
            </p>
        </section>

        <section class="card">
            <p class="eyebrow">Published versions</p>
            <h2>Version snapshots</h2>
            <ul class="list">
${versions_markup}            </ul>
        </section>
    </div>
</div>
</body>
</html>
EOF
}

copy_static_file "${ROOT_INDEX}" "${TARGET_DIR}/index.html"
copy_static_file "${ROOT_STYLES}" "${TARGET_DIR}/styles.css"
copy_static_file "${PLAYGROUND_INDEX}" "${TARGET_DIR}/playground/index.html"
mkdir -p "${TARGET_DIR}/api"

copy_tree "${PLAYGROUND_ARTIFACT_ROOT}" "${TARGET_DIR}/playground/latest"

copy_tree "${API_ARTIFACT_ROOT}" "${TARGET_DIR}/api/latest"

if [[ -n "${VERSION_TAG}" ]]; then
    NORMALIZED_VERSION_TAG="$(normalize_version_tag "${VERSION_TAG}")"

    copy_tree "${PLAYGROUND_ARTIFACT_ROOT}" "${TARGET_DIR}/playground/${NORMALIZED_VERSION_TAG}"

    copy_tree "${API_ARTIFACT_ROOT}" "${TARGET_DIR}/api/${NORMALIZED_VERSION_TAG}"
fi

API_VERSIONS=()
for api_subdir in "${TARGET_DIR}/api"/*; do
    if [[ ! -d "${api_subdir}" ]]; then
        continue
    fi
    api_name="$(basename "${api_subdir}")"
    if [[ "${api_name}" == "latest" ]]; then
        continue
    fi
    API_VERSIONS+=("${api_name}")
done

if [[ ${#API_VERSIONS[@]} -gt 0 ]]; then
    SORTED_API_VERSIONS=()
    while IFS= read -r version; do
        SORTED_API_VERSIONS+=("${version}")
    done < <(printf '%s\n' "${API_VERSIONS[@]}" | sort)
    API_VERSIONS=("${SORTED_API_VERSIONS[@]}")
fi

render_api_index_page "${TARGET_DIR}/api" "${API_VERSIONS[@]}"

touch "${TARGET_DIR}/.nojekyll"
