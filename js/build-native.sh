#!/bin/bash
set -e

# Build script for the native C API library

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/native/build"
DIST_DIR="${SCRIPT_DIR}/dist/native"

echo "Building remidy C API library..."

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure with CMake
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${DIST_DIR}"

# Build
cmake --build . --config Release -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Create dist directory and copy library
mkdir -p "${DIST_DIR}"

# Copy the library based on platform
if [[ "$OSTYPE" == "darwin"* ]]; then
    cp libremidy_c.dylib "${DIST_DIR}/"
    echo "Built: ${DIST_DIR}/libremidy_c.dylib"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    cp libremidy_c.so "${DIST_DIR}/"
    echo "Built: ${DIST_DIR}/libremidy_c.so"
elif [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" ]]; then
    cp Release/remidy_c.dll "${DIST_DIR}/"
    echo "Built: ${DIST_DIR}/remidy_c.dll"
fi

echo "Native library build complete!"
