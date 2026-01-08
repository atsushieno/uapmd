# uapmd-app WebAssembly Port - Current Status

**Last Updated**: January 11, 2026

## ✅ Build Infrastructure Complete

Successfully built minimal ImGui + GLFW WebAssembly application with full rendering capabilities.

### Build Environment
- **Emscripten SDK**: 4.0.22 (installed via git submodule at `external/emsdk`)
- **Backend**: GLFW3 + OpenGL ES 3.0 (switched from SDL2 for better Emscripten support)
- **ImGui Version**: 1.92.3

### Build Commands
```bash
# Setup Emscripten (one-time)
cd external/emsdk
./emsdk install latest
./emsdk activate latest

# Build WebAssembly application
cd web
source ../external/emsdk/emsdk_env.sh
./build-clean.sh

# Serve and test
npx serve -s /tmp/uapmd-wasm-build/build -l 8080
# Open http://localhost:8080
```

### Generated Output
- `uapmd-app.js` - JavaScript loader (~172KB)
- `uapmd-app.wasm` - WebAssembly binary (~576KB)
- `index.html` - HTML page with canvas

## What's Working

1. ✅ ImGui rendering in browser via GLFW + WebGL2
2. ✅ GLFW input handling (mouse, keyboard)
3. ✅ Emscripten main loop integration
4. ✅ Canvas rendering with OpenGL ES 3.0
5. ✅ Basic demo UI displaying successfully
6. ✅ Clean build system with portable paths

## What Needs To Be Done

### Phase 1: Minimal UI Port
- [x] Standalone ImGui build working
- [x] GLFW + Emscripten integration working
- [ ] Port actual uapmd-app GUI components
- [ ] Create web-specific main.cpp using uapmd headers
- [ ] Basic UI layout matching desktop version

### Phase 2: Stub Implementation
- [ ] Create stub AppModel for WebAssembly (no actual plugins)
- [ ] Stub audio backend (Web Audio API integration later)
- [ ] Stub MIDI backend (Web MIDI API integration later)
- [ ] Stub file I/O (IndexedDB/File System Access API later)
- [ ] Stub plugin scanning/loading

### Phase 3: Web API Integration
- [ ] Web Audio API for audio output
- [ ] Web MIDI API for MIDI input/output
- [ ] File System Access API for plugin loading
- [ ] IndexedDB for state persistence
- [ ] Web Workers for plugin processing

### Phase 4: Full uapmd-app Port
- [ ] Integrate all GUI components from tools/uapmd-app
- [ ] Port audio processing pipeline
- [ ] Port MIDI routing
- [ ] Add WebAssembly plugin format support (if feasible)

## Recent Changes (January 11, 2026)

### Fixed Issues
1. **SDL2 Compatibility**: Switched from SDL2 to GLFW backend
   - SDL2's Emscripten port lacks full game controller and system cursor support
   - GLFW has better Emscripten integration via `contrib.glfw3` port

2. **Hardcoded Paths**: Made all build scripts portable
   - Removed macOS-specific paths (`/Users/atsushi`, `/opt/homebrew`)
   - Use `$SCRIPT_DIR` and `$EMSDK` environment variables
   - Works on Linux, macOS, and Windows (WSL)

3. **Build System**: Improved build configuration
   - Uses CMake with Emscripten toolchain
   - Proper include paths for GLFW and ImGui backends
   - Optimized linker flags for WebAssembly

### Current Code Structure

#### `/web/CMakeLists.txt`
Full CMakeLists that:
- Fetches all dependencies (choc, cmidi2, midicci, cpplocate, ImGui, etc.)
- Builds remidy libraries
- Compiles uapmd-app with GLFW + OpenGL3 backends
- Uses Emscripten-specific flags (WASM, GLFW, WebGL2)

#### `/web/web_main_minimal.cpp`
Minimal working example with:
- GLFW window creation
- ImGui context setup
- Emscripten main loop integration
- Basic demo UI rendering

#### `/web/build-clean.sh`
Standalone build script that:
- Creates isolated build in `/tmp/uapmd-wasm-build/`
- Generates fresh CMakeLists.txt
- Builds with GLFW backend
- Creates index.html for testing

## Next Steps (Priority Order)

1. **Port GUI Components** (Next immediate task)
   - Copy MainWindow.cpp structure to web_main.cpp
   - Add stub AppModel that returns fake data
   - Integrate PluginList, ParameterList, etc. with mock data
   - Test UI responsiveness in browser

2. **Create Web-Specific Backends**
   - Stub AudioDevice that uses Web Audio API
   - Stub MidiDevice that uses Web MIDI API
   - Stub FileDialog using File System Access API

3. **Build Full Application**
   - Switch from web_main_minimal.cpp to full uapmd-app sources
   - Conditionally compile out desktop-only features
   - Add preprocessor guards for Emscripten-specific code

4. **Optimize and Deploy**
   - Minimize WASM binary size
   - Add loading progress indicator
   - Create deployment configuration for GitHub Pages

## Technical Notes

### Why GLFW Instead of SDL2?
Emscripten's SDL2 port is incomplete and missing:
- Game controller support (`SDL_GameController*`)
- System cursors (`SDL_SYSTEM_CURSOR_*`)
- Global mouse state (`SDL_GetGlobalMouseState`)
- Window display index (`SDL_GetWindowDisplayIndex`)

GLFW's Emscripten port (`contrib.glfw3`) is more complete and actively maintained.

### Build Artifacts Location
- Isolated builds: `/tmp/uapmd-wasm-build/build/`
- In-tree builds: `/sources/uapmd/web/build/`
- Both work, but isolated is cleaner for testing

### Testing the Application
```bash
# Build
cd /sources/uapmd/web
source ../external/emsdk/emsdk_env.sh
./build-clean.sh

# Serve
npx serve -s /tmp/uapmd-wasm-build/build -l 8080

# Open browser
# Navigate to http://localhost:8080
```

## References

- [Emscripten Documentation](https://emscripten.org/docs/)
- [GLFW with Emscripten](https://github.com/pongasoft/emscripten-glfw)
- [ImGui Emscripten Examples](https://github.com/ocornut/imgui/tree/master/examples/example_glfw_opengl3)
- [Web MIDI API](https://developer.mozilla.org/en-US/docs/Web/API/Web_MIDI_API)
- [Web Audio API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Audio_API)
