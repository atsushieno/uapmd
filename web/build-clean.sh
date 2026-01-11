#!/bin/bash

set -e

echo "=== Building uapmd-app for WebAssembly (Standalone) ==="

# Use completely isolated build directory
rm -rf /tmp/uapmd-wasm-build
mkdir -p /tmp/uapmd-wasm-build

# Copy CMakeLists and source files
cat > /tmp/uapmd-wasm-build/CMakeLists.txt <<'CMAKEEOF'
cmake_minimum_required(VERSION 3.21)
project(uapmd-wasm VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This CMakeLists.txt is for Emscripten/WebAssembly builds only")
endif()

message(STATUS "Building for Emscripten/Wasm (standalone)")

add_compile_definitions(EMSCRIPTEN=1)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -std=c++23 -s DISABLE_EXCEPTION_CATCHING=1 --use-port=contrib.glfw3")

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MAXIMUM_MEMORY=512MB \
    -s USE_WEBGL2=1 \
    -s FULL_ES3=1 \
    -s MIN_WEBGL_VERSION=2 \
    -s MAX_WEBGL_VERSION=2 \
    -s DISABLE_EXCEPTION_CATCHING=1 \
    --use-port=contrib.glfw3 \
    -s NO_EXIT_RUNTIME=0 \
    -s ASSERTIONS=1 \
    -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8']")

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.3
)
FetchContent_MakeAvailable(imgui)

add_executable(uapmd-app
    web_main_minimal.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(uapmd-app PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(uapmd-app PRIVATE IMGUI_IMPL_OPENGL_ES3=1)

find_package(Threads REQUIRED)
target_link_libraries(uapmd-app PRIVATE Threads::Threads)
CMAKEEOF

# Copy source files
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cp "$SCRIPT_DIR/web_main_minimal.cpp" /tmp/uapmd-wasm-build/

cd /tmp/uapmd-wasm-build

echo "Configuring..."
rm -rf build
mkdir build && cd build
emcmake cmake ..

echo "Building..."
cmake --build . --config Release

# Generate index.html
cat > index.html <<'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>uapmd-app (WebAssembly)</title>
    <style>
        body { 
            margin: 0; 
            padding: 0; 
            background: #1a1a1a; 
            overflow: hidden; 
            display: flex;
            justify-content: center;
            align-items: center;
            height: 100vh;
        }
        #loading {
            position: absolute;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            color: #ffffff;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            font-size: 24px;
            text-align: center;
        }
        canvas { 
            display: block; 
            box-shadow: 0 0 20px rgba(0,0,0,0.5);
        }
    </style>
</head>
<body>
    <div id="loading">
        Loading uapmd-app...
    </div>
    <canvas id="canvas"></canvas>
    
    <script>
        var Module = {
            canvas: document.getElementById('canvas'),
            preRun: [
                function() {
                    document.getElementById('loading').style.display = 'none';
                }
            ],
            printErr: function(text) {
                console.error('uapmd-app error:', text);
            },
            print: function(text) {
                console.log('uapmd-app:', text);
            }
        };
    </script>
    <script src="uapmd-app.js" type="text/javascript" async></script>
</body>
</html>
EOF

if [ -f "uapmd-app.js" ] && [ -f "uapmd-app.wasm" ]; then
    echo ""
    echo "✓ Build complete!"
    echo "  - build/uapmd-app.js ($(du -h uapmd-app.js | cut -f1))"
    echo "  - build/uapmd-app.wasm ($(du -h uapmd-app.wasm | cut -f1))"
    echo "  - build/index.html"
    echo ""
    echo "To test:"
    echo "  npx serve -s build -l 8080"
    echo ""
    echo "Then open: http://localhost:8080"
else
    echo "✗ Build failed"
    exit 1
fi
