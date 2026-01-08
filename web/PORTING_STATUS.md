# WebAssembly Porting Progress

**Date**: January 11, 2026

## ✅ Phase 1 Complete: Infrastructure Setup

Successfully set up the WebAssembly build infrastructure:

1. **Emscripten SDK**: Installed and configured via git submodule
2. **GLFW Backend**: Switched from SDL2 to GLFW for better Emscripten support
3. **Minimal Demo**: Working ImGui + GLFW application rendering in browser
4. **Build Scripts**: Portable build scripts working on Linux/macOS/Windows

## 🚧 Phase 2 In Progress: Full Application Port

### Current Status

Created `web_main.cpp` - a web-specific entry point that:
- Uses Emscripten's main loop (instead of desktop while loop)
- Integrates with existing Platform Backend abstraction
- Initializes AppModel, MainWindow, and all GUI components
- Wraps errors gracefully for missing functionality

### Blockers Identified

The full uapmd-app has significant desktop dependencies:

1. **X11**: Required by remidy and midicci libraries
2. **GTK+**: Required for file dialogs
3. **Platform Audio APIs**: CoreAudio/ALSA/WASAPI
4. **Desktop MIDI APIs**: CoreMIDI/ALSA MIDI
5. **Plugin Formats**: VST3, AU, LV2, CLAP (desktop only)

### Two Possible Approaches

#### Approach A: Stub Everything (Recommended for MVP)
Create WebAssembly-specific stub implementations:

**Pros**:
- Faster to get working
- Can test GUI components independently
- Incremental integration of Web APIs

**Cons**:
- Requires creating stub libraries
- More upfront work

**Steps**:
1. Create `src/remidy-web` - Web stubs for remidy
2. Create `src/uapmd-web` - Web stubs for uapmd
3. Skip plugin format libraries entirely
4. Build only GUI components + stubs

#### Approach B: Conditional Compilation (Cleaner long-term)
Add WebAssembly guards throughout existing code:

**Pros**:
- Cleaner code organization
- Easier to maintain

**Cons**:
- Requires modifying many existing files
- More complex build configuration

**Steps**:
1. Add `#ifdef EMSCRIPTEN` guards in remidy/uapmd
2. Disable X11/GTK/audio dependencies for web
3. Provide minimal web implementations inline

## 📋 Next Steps (Recommended: Approach A)

### Step 1: Create Stub Libraries

```cmake
# web/CMakeLists.txt additions

# Instead of:
# add_subdirectory(${UAPMD_ROOT}/src/remidy ...)

# Create web-specific stubs:
add_library(remidy-web STATIC
    stubs/remidy_stubs.cpp
)

add_library(uapmd-web STATIC
    stubs/uapmd_stubs.cpp
    stubs/audio_stubs.cpp
)
```

### Step 2: Minimal Stub Implementations

Create files that provide just enough to link:
- `stubs/remidy_stubs.cpp` - Empty plugin host functions
- `stubs/uapmd_stubs.cpp` - Stub MIDI device management
- `stubs/audio_stubs.cpp` - Stub audio I/O (later: Web Audio API)

### Step 3: Build GUI Only

Focus on getting MainWindow and GUI components working:
- PluginList (with mock data)
- ParameterList (with mock data)
- TrackList (with mock data)
- All visualization components

### Step 4: Incremental Web API Integration

Once GUI works with stubs, add real functionality:
1. Web Audio API for audio output
2. Web MIDI API for MIDI I/O
3. File System Access API for file loading
4. IndexedDB for state persistence

## 🎯 Immediate Next Action

**Create stub implementations** to unblock the build:

```bash
mkdir -p web/stubs
# Create minimal stub files
# Update CMakeLists.txt to use stubs instead of full libraries
# Build and test
```

## Files Modified

1. ✅ `/sources/uapmd/web/web_main.cpp` - Web-specific entry point
2. ✅ `/sources/uapmd/web/CMakeLists.txt` - Updated for full app build
3. ✅ `/sources/uapmd/web/build-clean.sh` - GLFW backend, portable paths
4. ✅ `/sources/uapmd/web/README.md` - Updated documentation
5. ✅ `/sources/uapmd/web/WASM_PORT_STATUS.md` - Detailed status

## Test Results

- ✅ Minimal ImGui demo: **Working**
- ✅ WebAssembly build: **Successfully compiling**
- ⏳ Full uapmd-app: **Deferred - requires stub implementations**
- ⏳ Web Audio integration: **Not started**
- ⏳ Web MIDI integration: **Not started**

## Build Status (January 11, 2026)

Successfully built a minimal WebAssembly version of uapmd-app with ImGui + GLFW!

### What Works
- Emscripten SDK integration
- GLFW3 windowing backend (via contrib.glfw3 port)
- ImGui rendering to WebGL2
- Basic application loop

### Files Generated
- `build/uapmd-app.wasm` - WebAssembly binary (577KB)
- `build/uapmd-app.js` - JavaScript loader (171KB)
- `build/index.html` - Test HTML page

### To Test
```bash
cd /sources/uapmd/web/build
python3 -m http.server 8080
# Open http://localhost:8080 in your browser
```

## Timeline Estimate

With stub approach:
- Stubs creation: ~2-4 hours
- GUI integration: ~4-8 hours
- Web APIs: ~8-16 hours
- Testing & polish: ~4-8 hours

**Total**: ~18-36 hours of focused work

## Resources Needed

1. Understanding of uapmd library interfaces
2. Web Audio API knowledge
3. Web MIDI API knowledge
4. Emscripten best practices
5. ImGui web deployment experience
