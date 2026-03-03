#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${REPO_ROOT}/cmake-build-ios-sim"
BUNDLE_ID="org.uapmd.app"

ARCH="${UAPMD_IOS_ARCH:-arm64}"
DEPLOYMENT_TARGET="${UAPMD_IOS_DEPLOYMENT_TARGET:-16.4}"
CONFIGURATION="${UAPMD_IOS_CONFIGURATION:-Debug}"
SIMULATOR_NAME="${UAPMD_IOS_SIMULATOR:-}"

CPM_CACHE_ARG=""
SKIP_CONFIGURE=false
SKIP_BUILD=false
SKIP_LAUNCH=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-configure)
            SKIP_CONFIGURE=true
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --skip-launch)
            SKIP_LAUNCH=true
            shift
            ;;
        --simulator)
            SIMULATOR_NAME="$2"
            shift 2
            ;;
        --configuration)
            CONFIGURATION="$2"
            shift 2
            ;;
        --help|-h)
            cat <<HELP
Usage: ./build-ios-sim.sh [options] [CPM_SOURCE_CACHE=<path>]

Options:
  --simulator NAME       Simulator name to target (default: auto-select latest iPhone)
  --configuration CFG    Build configuration: Debug or Release (default: Debug)
  --skip-configure       Skip cmake configure step
  --skip-build           Skip cmake build step
  --skip-launch          Configure and build only; do not install or launch

Environment variables:
  CPM_SOURCE_CACHE       Path to CPM package cache (or pass as CPM_SOURCE_CACHE=<path>)
  UAPMD_IOS_SIMULATOR    Default simulator name
  UAPMD_IOS_CONFIGURATION  Default build configuration
HELP
            exit 0
            ;;
        CPM_SOURCE_CACHE=*)
            CPM_CACHE_ARG="-DCPM_SOURCE_CACHE=${1#CPM_SOURCE_CACHE=}"
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "${CPM_CACHE_ARG}" && -n "${CPM_SOURCE_CACHE:-}" ]]; then
    CPM_CACHE_ARG="-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}"
fi

configure() {
    echo "Configuring for iOS Simulator (${ARCH}, deployment target ${DEPLOYMENT_TARGET})..."
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Xcode \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO \
        -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="" \
        ${CPM_CACHE_ARG}
}

build() {
    echo "Building uapmd-app (${CONFIGURATION})..."
    cmake --build "${BUILD_DIR}" \
        --config "${CONFIGURATION}" \
        --target uapmd-app \
        -- -sdk iphonesimulator
}

find_simulator_udid() {
    local target_name="${SIMULATOR_NAME}"
    xcrun simctl list devices available -j | python3 -c "
import json, sys
data = json.load(sys.stdin)
target_name = '${target_name}'
best_udid = ''
best_runtime = ''
for runtime, devices in data['devices'].items():
    if 'com.apple.CoreSimulator.SimRuntime.iOS' not in runtime:
        continue
    for d in devices:
        if not d.get('isAvailable', False):
            continue
        if target_name:
            if d['name'] == target_name:
                print(d['udid'])
                exit(0)
        else:
            if 'iPhone' in d['name'] and runtime > best_runtime:
                best_runtime = runtime
                best_udid = d['udid']
if best_udid:
    print(best_udid)
"
}

launch() {
    local udid
    udid="$(find_simulator_udid)"
    if [[ -z "${udid}" ]]; then
        echo "error: no suitable iOS simulator found" >&2
        echo "  Run: xcrun simctl list devices available" >&2
        [[ -n "${SIMULATOR_NAME}" ]] && echo "  Requested: '${SIMULATOR_NAME}'" >&2
        exit 1
    fi

    local app_path
    app_path="$(find "${BUILD_DIR}/source/tools/uapmd-app/${CONFIGURATION}-iphonesimulator" \
        -maxdepth 1 -name "uapmd-app.app" 2>/dev/null | head -1)"
    if [[ -z "${app_path}" ]]; then
        echo "error: uapmd-app.app not found under ${BUILD_DIR}" >&2
        echo "  Expected: ${BUILD_DIR}/source/tools/uapmd-app/${CONFIGURATION}-iphonesimulator/uapmd-app.app" >&2
        exit 1
    fi

    echo "Booting simulator ${udid}..."
    xcrun simctl boot "${udid}" 2>/dev/null || true   # already booted is OK
    open -a Simulator

    echo "Installing ${app_path}..."
    xcrun simctl install "${udid}" "${app_path}"

    echo "Launching ${BUNDLE_ID}..."
    xcrun simctl launch --console "${udid}" "${BUNDLE_ID}"
}

${SKIP_CONFIGURE} || configure
${SKIP_BUILD}     || build
${SKIP_LAUNCH}    || launch

echo "✓ Done"
