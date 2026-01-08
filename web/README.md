# uapmd-app WebAssembly Build

**Status**: ✅ **Build working!** Minimal ImGui demo successfully running in browser

## Quick Start

### Prerequisites
- Linux, macOS, or Windows (WSL)
- CMake 3.21+
- Node.js and npm (for serving)

### Build & Run (3 Steps)

#### 1. Setup Emscripten (one-time)
```bash
cd /sources/uapmd

# Initialize and setup Emscripten SDK from submodule
cd external/emsdk
./emsdk install latest
./emsdk activate latest

# Return to project root
cd ../..
```

#### 2. Build WebAssembly
```bash
cd web
source ../external/emsdk/emsdk_env.sh
./build-clean.sh
```

**Output files** (in `/tmp/uapmd-wasm-build/build/`):
- `uapmd-app.js` - JavaScript loader (~172KB)
- `uapmd-app.wasm` - WebAssembly binary (~576KB)
- `index.html` - HTML page with canvas

#### 3. Test in Browser
```bash
npx serve -s /tmp/uapmd-wasm-build/build -l 8080
```

Open http://localhost:8080 in your browser

## What's Working

✅ ImGui rendering in browser via GLFW + WebGL2
✅ GLFW input handling (mouse, keyboard)
✅ Emscripten main loop integration
✅ Canvas rendering with OpenGL ES 3.0
✅ Basic demo UI displaying successfully
✅ Clean build system with portable paths

## Architecture

```
web_main_minimal.cpp (C++ with ImGui+GLFW)
         ↓
    emcc (Emscripten compiler)
         ↓
uapmd-app.wasm + uapmd-app.js
         ↓
  Browser (Canvas + WebGL2)
```

### Tech Stack
- **Backend**: GLFW3 (switched from SDL2 for better Emscripten support)
- **Rendering**: OpenGL ES 3.0 → WebGL2
- **UI**: ImGui 1.92.3
- **Compiler**: Emscripten 4.0.22

### Why GLFW Instead of SDL2?
SDL2's Emscripten port is incomplete, missing:
- Game controller support
- System cursors
- Global mouse state
- Window display index

GLFW's `contrib.glfw3` port is more complete and actively maintained.

## Build Scripts

| Script | Purpose |
|--------|---------|
| `build-clean.sh` | ✅ **Recommended** - Isolated build in `/tmp/` |
| `build-standalone.sh` | In-tree build in `web/build/` |
| `build-full.sh` | Full build with error checking |
| `build-simple.sh` | Simple build without extras |

## Development Files

| File | Description |
|------|-------------|
| `CMakeLists.txt` | Main build configuration |
| `web_main_minimal.cpp` | Minimal GLFW+ImGui example |
| `WASM_PORT_STATUS.md` | Detailed status and next steps |
| `BUILD.md` | Build instructions |

## Next Steps

See [WASM_PORT_STATUS.md](WASM_PORT_STATUS.md) for:
- Phase 1: Port uapmd-app GUI components
- Phase 2: Add stub implementations
- Phase 3: Integrate Web APIs (Audio, MIDI, File System)
- Phase 4: Full uapmd-app port

## Troubleshooting

### Build fails
```bash
# Make sure Emscripten is activated
source ../external/emsdk/emsdk_env.sh

# Check version
emcc --version  # Should be 4.0.22

# Clean and rebuild
rm -rf /tmp/uapmd-wasm-build
./build-clean.sh
```

### Browser shows blank page
1. Check browser console (F12) for errors
2. Verify files exist: `ls /tmp/uapmd-wasm-build/build/`
3. Try a different browser (Chrome, Firefox work best)

### "Module not found" errors
Make sure you're serving from the correct directory:
```bash
npx serve -s /tmp/uapmd-wasm-build/build -l 8080
```

## Browser Support

| Browser | Status | Notes |
|---------|--------|-------|
| Chrome/Edge | ✅ Full | Recommended |
| Firefox | ✅ Full | Works great |
| Safari | ✅ Good | Some limitations |
| Mobile | ⚠️ Limited | Touch only |

## Resources

- [WASM_PORT_STATUS.md](WASM_PORT_STATUS.md) - Detailed status
- [Emscripten Docs](https://emscripten.org/docs/)
- [GLFW for Emscripten](https://github.com/pongasoft/emscripten-glfw)
- [ImGui Examples](https://github.com/ocornut/imgui/tree/master/examples/example_glfw_opengl3)

---

**Status**: Basic infrastructure working. Next: Port actual uapmd-app GUI components.
