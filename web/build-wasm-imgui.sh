#!/bin/bash

# Build script for compiling uapmd-app to WebAssembly with ImGui+SDL2

set -e

echo "=== Building uapmd-app for WebAssembly (ImGui+SDL2) ==="

# Check for Emscripten
if ! command -v emcmake &> /dev/null; then
    echo "Error: Emscripten not found!"
    echo ""
    echo "Install Emscripten:"
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk"
    echo "  ./emsdk install latest"
    echo "  ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    exit 1
fi

# Check for native build
NATIVE_BUILD_DIR="../cmake-build-emscripten"
if [ ! -d "$NATIVE_BUILD_DIR" ]; then
    echo "Error: Native build not found at $NATIVE_BUILD_DIR"
    echo "Please build the native version first:"
    echo "  cmake -B cmake-build-emscripten -G Ninja"
    echo "  cmake --build cmake-build-emscripten"
    exit 1
fi

# Build directory
BUILD_DIR="build-wasm-imgui"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

echo "Configuring with Emscripten (standalone)..."
emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DUAPMD_BUILD_WASM=ON \
    -DBUILD_SHARED_LIBS=OFF

echo ""
echo "Building..."
cmake --build . --config Release

if [ -f "uapmd-app.html" ] && [ -f "uapmd-app.js" ] && [ -f "uapmd-app.wasm" ]; then
    echo ""
    echo "✓ Build complete!"
    echo "✓ Output files:"
    echo "  - uapmd-app.html"
    echo "  - uapmd-app.js"
    echo "  - uapmd-app.wasm"
    echo ""
    echo "To run the web app:"
    echo "  cd .."
    echo "  npx serve -s $BUILD_DIR -l 8080"
    echo ""
    echo "Then open http://localhost:8080 in your browser"
else
    echo "✗ Build failed - expected files not found"
    exit 1
fi
