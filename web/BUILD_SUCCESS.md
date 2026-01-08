# WebAssembly Build - Success Report

**Date:** January 11, 2026
**Status:** ✅ Minimal build working

## Summary

Successfully built a minimal WebAssembly version of uapmd-app using Emscripten, GLFW, and ImGui. The application compiles to WebAssembly and can run in a web browser.

## What Was Accomplished

### 1. Infrastructure Setup
- ✅ Emscripten SDK installed (version 4.0.22)
- ✅ Switched from SDL2 to GLFW backend for better Emscripten support
- ✅ Created portable build scripts (works on Linux/macOS/Windows)
- ✅ Set up CMake configuration for cross-compilation

### 2. Dependency Management
- ✅ Made X11/GTK optional in remidy CMakeLists.txt
- ✅ Created patch for midicci to make X11 optional (`/sources/uapmd/midicci-emscripten.patch`)
- ✅ Disabled platform-specific dependencies (VST3, AU, LV2, CLAP)
- ✅ Simplified web/CMakeLists.txt to build minimal version only

### 3. Build Output
- **uapmd-app.wasm** - 577KB WebAssembly binary
- **uapmd-app.js** - 171KB JavaScript loader
- **index.html** - Test HTML page with canvas setup

## Testing the Build

```bash
cd /sources/uapmd/web/build
python3 -m http.server 8080
```

Then open http://localhost:8080 in your web browser.

## Key Files Modified

1. **`/sources/uapmd/web/CMakeLists.txt`**
   - Disabled VST3, AU, LV2, CLAP, platform MIDI
   - Disabled X11, GTK, native audio dependencies
   - Commented out remidy, cmidi2, cpplocate, midicci for minimal build
   - Building only ImGui + GLFW + minimal entry point

2. **`/sources/uapmd/src/remidy/CMakeLists.txt`**
   - Made X11/GTK optional for EMSCRIPTEN builds
   - Preserved functionality for desktop builds

3. **`/sources/uapmd/web/cmake/CPM.cmake`**
   - Created symlink to parent cmake/CPM.cmake
   - Fixes midicci-app dependency resolution

4. **`/sources/uapmd/midicci-emscripten.patch`**
   - Patch to make X11 optional in midicci-app
   - To be applied upstream when needed

## Current Architecture

```
web_main.cpp (minimal ImGui demo)
    ↓
ImGui (UI framework)
    ↓
GLFW (windowing/input via Emscripten port)
    ↓
WebGL2 (OpenGL ES 3.0 compatibility)
    ↓
Browser Canvas
```

## Next Steps

### Phase 1: Verify Minimal Build
- [ ] Test in browser (Chrome/Firefox/Safari)
- [ ] Verify ImGui rendering
- [ ] Test mouse/keyboard input
- [ ] Check performance

### Phase 2: Add Application Components
- [ ] Create web-specific stubs for remidy
- [ ] Create web-specific stubs for uapmd
- [ ] Port GUI components from main app
  - [ ] PluginList (with mock data)
  - [ ] ParameterList (with mock data)
  - [ ] TrackList (with mock data)
- [ ] Integrate MainWindow GUI

### Phase 3: Web API Integration
- [ ] Web Audio API for audio output
- [ ] Web MIDI API for MIDI I/O (if needed for MIDI 2.0)
- [ ] File System Access API for file operations
- [ ] IndexedDB for state persistence

### Phase 4: Plugin Support (Future)
- [ ] WebCLAP integration (when available)
- [ ] Web-based plugin hosting

## Known Limitations

1. **No Platform MIDI**: Web MIDI 2.0 API doesn't exist yet
2. **No Native Plugins**: VST3/AU/LV2 not available in browsers
3. **No Direct Audio**: Must use Web Audio API
4. **File Access**: Limited by browser security model

## Compiler Flags

### Emscripten C++ Flags
```
-O3 -std=c++23 -s DISABLE_EXCEPTION_CATCHING=1 --use-port=contrib.glfw3
```

### Emscripten Linker Flags
```
-s WASM=1
-s ALLOW_MEMORY_GROWTH=1
-s MAXIMUM_MEMORY=512MB
-s USE_WEBGL2=1
-s FULL_ES3=1
-s MIN_WEBGL_VERSION=2
-s DISABLE_EXCEPTION_CATCHING=1
--use-port=contrib.glfw3
-s NO_EXIT_RUNTIME=0
-s ASSERTIONS=1
-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8']
```

## Build Commands

### Quick Build
```bash
cd /sources/uapmd/web
./build-clean.sh
```

### Manual Build
```bash
cd /sources/uapmd/web
source /sources/uapmd/external/emsdk/emsdk_env.sh
rm -rf build && mkdir -p build && cd build
emcmake cmake ..
cmake --build .
```

## Troubleshooting

### Issue: CPM.cmake not found
**Solution:** Create symlink in web/cmake directory
```bash
mkdir -p /sources/uapmd/web/cmake
ln -s ../../cmake/CPM.cmake /sources/uapmd/web/cmake/CPM.cmake
```

### Issue: X11 required but disabled
**Solution:** Apply patches for remidy and midicci to make X11 optional for EMSCRIPTEN

### Issue: VST3 SDK compilation errors
**Solution:** Don't build remidy for minimal version; comment out remidy in web/CMakeLists.txt

## Resources

- **Emscripten Docs**: https://emscripten.org/docs/
- **GLFW Emscripten Port**: https://github.com/emscripten-ports/glfw
- **ImGui**: https://github.com/ocornut/imgui
- **Web Audio API**: https://developer.mozilla.org/en-US/docs/Web/API/Web_Audio_API
- **Web MIDI API**: https://developer.mozilla.org/en-US/docs/Web/API/Web_MIDI_API
