# uapmd-app for Web using ImGui+SDL2

## Overview

Instead of creating a custom HTML/JS UI, we can compile the existing `uapmd-app` with ImGui+SDL2 directly to WebAssembly using Emscripten. SDL2's Emscripten port provides:
- Canvas rendering
- Input handling (mouse, keyboard, touch)
- Web Audio API integration
- WebGL context management
- Fullscreen support

## Architecture

```
uapmd-app (C++ with ImGui)
    ↓ Emscripten
WebAssembly + SDL2 WebGL backend
    ↓ Runs in
Browser with identical UI
```

## Key Components

### SDL2 Emscripten Features
- **`-s USE_SDL=2`** - Use SDL2 port
- **Canvas rendering** - ImGui renders to WebGL canvas
- **Input handling** - Mouse, keyboard, touch events
- **Audio integration** - Web Audio API for plugin audio
- **File access** - File System Access API for plugins

### ImGui in Browser
- Immediate mode rendering works identically to native
- All existing uapmd-app UI code preserved
- No HTML/CSS needed for interface
- Same look and feel across platforms

## Implementation Plan

### Phase 1: Basic Wasm Build
1. Test Emscripten installation
2. Create SDL2+Emscripten build configuration
3. Compile minimal ImGui test to Wasm
4. Verify canvas rendering works

### Phase 2: Audio Integration
1. Integrate Web Audio API with SDL2 audio backend
2. Create AudioWorklet for real-time processing
3. Connect to remidy audio pipeline

### Phase 3: Plugin Loading
1. Implement File System Access API for plugin files
2. Create plugin file upload mechanism
3. Integrate with remidy plugin loading

### Phase 4: MIDI Support
1. Connect Web MIDI API to SDL2 MIDI backend
2. Route MIDI events to ImGui event loop
3. Display MIDI data in ImGui UI

## Build Configuration

### CMake for Emscripten

```cmake
# web/CMakeLists.txt
cmake_minimum_required(VERSION 3.21)
project(uapmd-wasm VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Emscripten-specific flags
if(${EMSCRIPTEN})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -s USE_SDL=2 -std=c++23")
    set(CMAKE_EXE_LINKER_FLAGS 
        "${CMAKE_EXE_LINKER_FLAGS} \
        -s WASM=1 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s MAXIMUM_MEMORY=256MB \
        -s USE_WEBGL2=1 \
        -s FULL_ES3=1 \
        -s MIN_WEBGL_VERSION=2 \
        --shell-file index.html")
    
    # Copy index.html to build directory
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/index.html 
                     ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
else()
    # Native build
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

# Find libraries
 include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake-build-emscripten/_deps/choc-src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake-build-emscripten/_deps/cmidi2-src/include
)

# Source files
add_executable(uapmd-app
    ../../tools/uapmd-app/main.cpp
    # Add all uapmd and remidy source files...
)

# Link libraries
if(${EMSCRIPTEN})
    # Emscripten-specific libraries
    target_link_libraries(uapmd-app 
        ${SDL2_LIBRARY}
        ${OPENGL_LIBRARY})
else()
    # Native libraries
    target_link_libraries(uapmd-app 
        remidy
        remidy-tooling
        uapmd
        ${SDL2_LIBRARIES}
        ${OPENGL_LIBRARIES})
endif()
```

### Build Script

```bash
#!/bin/bash
# web/build-wasm-imgui.sh

set -e

# Check for Emscripten
if ! command -v emcmake &> /dev/null; then
    echo "Error: Emscripten not found"
    echo "Install: git clone https://github.com/emscripten-core/emsdk.git"
    exit 1
fi

# Build directory
BUILD_DIR="build-wasm-imgui"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

# Configure with Emscripten
emcmake cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"

# Build
cmake --build . --config Release

echo "✓ Build complete!"
echo "Open: ${BUILD_DIR}/uapmd-app.html"
```

### index.html Template

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
    <title>uapmd-app (WebAssembly)</title>
    <style>
        body { 
            margin: 0; 
            padding: 0; 
            background: #1a1a1a; 
            overflow: hidden; 
        }
        canvas { 
            display: block; 
            width: 100vw; 
            height: 100vh; 
        }
        #loading {
            position: fixed;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            color: white;
            font-family: system-ui, sans-serif;
            font-size: 24px;
        }
    </style>
</head>
<body>
    <div id="loading">Loading uapmd-app...</div>
    <canvas id="canvas"></canvas>
    
    <script>
        Module = {
            canvas: document.getElementById('canvas'),
            preRun: [
                function() {
                    document.getElementById('loading').style.display = 'none';
                }
            ],
            printErr: function(text) {
                console.error(text);
            }
        };
    </script>
    {{{ SCRIPT }}}
</body>
</html>
```

## Differences from Desktop Version

### Audio
- **Desktop**: Uses CoreAudio/ALSA/WASAPI
- **Web**: Uses Web Audio API through SDL2 Emscripten backend
- **Implementation**: AudioWorklet for low-latency processing

### File I/O
- **Desktop**: Native filesystem access
- **Web**: File System Access API + IndexedDB for caching

### MIDI
- **Desktop**: CoreMIDI/ALSA
- **Web**: Web MIDI API
- **Implementation**: SDL2 Web MIDI backend

### Windowing
- **Desktop**: SDL2 creates native window
- **Web**: SDL2 creates HTML5 canvas
- **Result**: Same ImGui rendering code works identically

## Benefits of This Approach

1. **Code Reuse** - 99% of uapmd-app code unchanged
2. **Consistent UI** - Identical ImGui interface
3. **Easier Maintenance** - Single codebase for all platforms
4. **Better Performance** - Native ImGui rendering
5. **Feature Parity** - Same features on all platforms

## Limitations

1. **Browser Security** - Audio requires user gesture to start
2. **File Access** - Users must explicitly grant file permissions
3. **Memory** - Limited to 256MB initially (can grow)
4. **Plugins** - Must be pre-downloaded by user (no system plugin scanning)

## Next Steps

1. **Install Emscripten** (if not already installed)
2. **Create web/CMakeLists.txt** with SDL2+Emscripten configuration
3. **Build minimal test** to verify ImGui rendering works
4. **Integrate remidy** library with Emscripten
5. **Add audio/MIDI/Web API bridges**
6. **Test plugin loading** with File System Access API

## Testing

```bash
# Build
cd web
./build-wasm-imgui.sh

# Serve
npx serve -s build-wasm-imgui -l 8080

# Open browser
# Navigate to http://localhost:8080
```

## References

- [SDL2 with Emscripten](https://wiki.libsdl.org/SDL2/SupportedPlatforms)
- [ImGui in WebAssembly](https://github.com/schteppe/imgui-wasm)
- [Emscripten SDL2 Documentation](https://emscripten.org/docs/api_reference/html5.h.html)
- [Web Audio API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Audio_API)
- [Web MIDI API](https://developer.mozilla.org/en-US/docs/Web/API/Web_MIDI_API)
- [File System Access API](https://developer.mozilla.org/en-US/docs/Web/API/File_System_Access_API)
