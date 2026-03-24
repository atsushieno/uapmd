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
PLAYGROUND_LATEST_INDEX="${SOURCE_DIR}/playground/latest/index.html"
PLAYGROUND_LATEST_SW="${SOURCE_DIR}/playground/latest/coop-coep-sw.js"
API_INDEX="${SOURCE_DIR}/api/index.html"
API_LATEST_INDEX="${SOURCE_DIR}/api/latest/index.html"

if [[ ! -f "${ROOT_INDEX}" || ! -f "${ROOT_STYLES}" ]]; then
    echo "error: missing GitHub Pages source files under ${SOURCE_DIR}" >&2
    exit 1
fi

if [[ ! -d "${ARTIFACTS_DIR}" ]]; then
    echo "error: artifacts directory does not exist: ${ARTIFACTS_DIR}" >&2
    exit 1
fi

if [[ ! -f "${ARTIFACTS_DIR}/uapmd-app.js" ]]; then
    echo "error: missing wasm artifact ${ARTIFACTS_DIR}/uapmd-app.js" >&2
    exit 1
fi

mkdir -p "${TARGET_DIR}" \
    "${TARGET_DIR}/playground" \
    "${TARGET_DIR}/api"

copy_static_file() {
    local src="$1"
    local dst="$2"

    mkdir -p "$(dirname "${dst}")"
    cp "${src}" "${dst}"
}

copy_bundle_files() {
    local dst_dir="$1"

    mkdir -p "${dst_dir}"
    find "${ARTIFACTS_DIR}" -maxdepth 1 -type f ! -name 'index.html' -print0 \
        | while IFS= read -r -d '' file; do
            cp "${file}" "${dst_dir}/"
        done
}

render_playground_version_page() {
    local version="$1"
    local dst="$2"

    cat > "${dst}" <<EOF
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <title>uapmd playground / ${version}</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="../../styles.css">
</head>
<body>
<div class="shell">
    <nav class="nav">
        <a class="brand" href="../../">uapmd web</a>
        <div class="nav-links">
            <a class="nav-link" href="../">Playground</a>
            <a class="nav-link" href="../../api/">API references</a>
        </div>
    </nav>

    <section class="hero">
        <p class="eyebrow">Release playground</p>
        <h1>uapmd-app ${version}</h1>
        <p class="lede">
            This is the published browser snapshot for release tag <code>${version}</code>.
        </p>
    </section>

    <section class="panel canvas-shell">
        <div id="status" class="status">Checking runtime files…</div>
        <canvas id="canvas"></canvas>
    </section>
</div>

<script>
    const statusNode = document.getElementById('status');

    function setStatus(text) {
        statusNode.textContent = text;
    }

    async function main() {
        try {
            const response = await fetch('./uapmd-app.js', { method: 'HEAD', cache: 'no-store' });
            if (!response.ok) {
                setStatus('uapmd-app.js is not published here yet.');
                return;
            }
        } catch (error) {
            setStatus('Failed to probe the Emscripten bundle. Check the browser console for details.');
            console.error('[uapmd-pages] probe failed', error);
            return;
        }

        if ('serviceWorker' in navigator) {
            try {
                await navigator.serviceWorker.register('./coop-coep-sw.js');
                if (!crossOriginIsolated) {
                    setStatus('Enabling cross-origin isolation and reloading…');
                    window.location.reload();
                    return;
                }
            } catch (error) {
                setStatus('Service worker registration failed. SharedArrayBuffer may be unavailable.');
                console.error('[uapmd-pages] service worker registration failed', error);
                return;
            }
        }

        if (!crossOriginIsolated) {
            setStatus('Cross-origin isolation is required for the current WebAssembly runtime.');
            return;
        }

        setStatus('Loading uapmd-app…');
        window.Module = {
            canvas: document.getElementById('canvas'),
            preRun: [() => {
                statusNode.style.display = 'none';
            }],
            print: (text) => console.log('[uapmd]', text),
            printErr: (text) => console.error('[uapmd]', text)
        };

        const script = document.createElement('script');
        script.src = './uapmd-app.js';
        script.onerror = () => {
            setStatus('The runtime bundle could not be loaded. Check that all published files are present.');
        };
        document.body.appendChild(script);
    }

    main();
</script>
</body>
</html>
EOF
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

rm -rf "${TARGET_DIR}/playground/latest"
mkdir -p "${TARGET_DIR}/playground/latest"
copy_static_file "${PLAYGROUND_LATEST_INDEX}" "${TARGET_DIR}/playground/latest/index.html"
copy_static_file "${PLAYGROUND_LATEST_SW}" "${TARGET_DIR}/playground/latest/coop-coep-sw.js"
copy_bundle_files "${TARGET_DIR}/playground/latest"

rm -rf "${TARGET_DIR}/api/latest"
mkdir -p "${TARGET_DIR}/api/latest"
copy_static_file "${API_LATEST_INDEX}" "${TARGET_DIR}/api/latest/index.html"

if [[ -n "${VERSION_TAG}" ]]; then
    SAFE_VERSION_TAG="${VERSION_TAG//\//-}"

    rm -rf "${TARGET_DIR}/playground/${SAFE_VERSION_TAG}"
    mkdir -p "${TARGET_DIR}/playground/${SAFE_VERSION_TAG}"
    render_playground_version_page "${SAFE_VERSION_TAG}" "${TARGET_DIR}/playground/${SAFE_VERSION_TAG}/index.html"
    copy_bundle_files "${TARGET_DIR}/playground/${SAFE_VERSION_TAG}"

    rm -rf "${TARGET_DIR}/api/${SAFE_VERSION_TAG}"
    mkdir -p "${TARGET_DIR}/api/${SAFE_VERSION_TAG}"
    render_api_version_page "${SAFE_VERSION_TAG}" "${TARGET_DIR}/api/${SAFE_VERSION_TAG}/index.html"
fi

touch "${TARGET_DIR}/.nojekyll"
