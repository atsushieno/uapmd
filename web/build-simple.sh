#!/bin/bash

# Simple standalone build script for uapmd-app WebAssembly

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building uapmd-app for WebAssembly (standalone) ==="

# Clean previous builds
rm -rf build
mkdir -p build
cd build

echo "Configuring with Emscripten..."
emcmake cmake ..

echo ""
echo "Building..."
cmake --build . --config Release

if [ -f "uapmd-app.js" ] && [ -f "uapmd-app.wasm" ]; then
    echo ""
    echo "✓ Build complete!"
    echo "✓ Output files:"
    echo "  - build/uapmd-app.js"
    echo "  - build/uapmd-app.wasm"
    echo ""
    echo "To run the web app:"
    echo "  npx serve -s build -l 8080"
    echo ""
    echo "Then open http://localhost:8080 in your browser"
else
    echo "✗ Build failed - expected files not found"
    exit 1
fi
