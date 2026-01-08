#!/bin/bash

# Minimal build script for uapmd WebAssembly stub
# This compiles a simple stub without requiring the full remidy library

set -e

echo "=== Building uapmd WebAssembly stub ==="

# Check if Emscripten is available
if ! command -v emcc &> /dev/null; then
    echo "Error: Emscripten not found!"
    echo ""
    echo "To install Emscripten:"
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk"
    echo "  ./emsdk install latest"
    echo "  ./emsdk activate latest"
    echo "  source ./emsdk_env.sh"
    exit 1
fi

# Build directory
DIST_DIR="dist"

# Clean previous builds
echo "Cleaning previous builds..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Compile the Wasm stub
echo "Compiling WebAssembly stub..."
cd "$(dirname "$0")"

emcc -s WASM=1 \
     -s MODULARIZE=1 \
     -s EXPORT_NAME="createUapmd" \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s MAXIMUM_MEMORY=64MB \
     -s "EXPORTED_RUNTIME_METHODS=['cwrap','getValue','setValue','addFunction','removeFunction','UTF8ToString','stringToUTF8']" \
     -s "EXPORTED_FUNCTIONS=['_malloc','_free','_uapmd_create','_uapmd_destroy','_uapmd_scan_plugins','_uapmd_get_plugin_count','_uapmd_get_plugin_name','_uapmd_get_plugin_vendor','_uapmd_get_plugin_format','_uapmd_get_plugin_path','_uapmd_load_plugin','_uapmd_start_audio','_uapmd_stop_audio','_uapmd_process_audio','_uapmd_get_parameter_count','_uapmd_get_parameter_name','_uapmd_get_parameter_value','_uapmd_set_parameter_value','_uapmd_send_midi']" \
     -I src \
     src/uapmd_wasm.cpp \
     -o "$DIST_DIR/uapmd.js" \
     -O3

if [ -f "$DIST_DIR/uapmd.wasm" ]; then
    echo "✓ WebAssembly stub compiled successfully!"
    echo "✓ Output files:"
    echo "  - $DIST_DIR/uapmd.js"
    echo "  - $DIST_DIR/uapmd.wasm"
    echo ""
    echo "To test the web interface:"
    echo "  npm install"
    echo "  npm run serve"
    echo ""
    echo "Then open http://localhost:8080 in your browser"
else
    echo "✗ Compilation failed"
    exit 1
fi
