# Build Instructions for uapmd-app WebAssembly

## Quick Start (Recommended)

Use the clean build script (no X11 dependency):

```bash
cd /Users/atsushi/sources/uapmd/web
./build-clean.sh
```

This creates a completely isolated build in `/tmp/uapmd-wasm-build/` and compiles:
- ✅ No X11 or midicci dependencies
- ✅ Clean build each time
- ✅ Generates `index.html` automatically
- ✅ Shows file sizes

## Test in Browser

After building successfully:

```bash
npx serve -s /tmp/uapmd-wasm-build/build -l 8080
```

Then open http://localhost:8080

## What This Builds

- ✅ ImGui with SDL2 backend
- ✅ WebGL2 rendering
- ✅ Emscripten main loop
- ✅ Basic demo UI
- ✅ Mouse and keyboard input
- ✅ Proper HTML loading screen
- ❌ No uapmd functionality yet (stub only)

## Output Files

Generated in `/tmp/uapmd-wasm-build/build/`:
- `uapmd-app.js` (~252KB) - JavaScript loader
- `uapmd-app.wasm` (~972KB) - WebAssembly binary
- `index.html` - HTML page with canvas and Module loader

## Troubleshooting

### "Loading uapmd-app..." stuck

If you see the loading message and nothing else:

1. **Check browser console** (F12) for errors:
   - Look for "Failed to load WASM" errors
   - Check if `uapmd-app.js` and `uapmd-app.wasm` are loading

2. **Verify files exist**:
   ```bash
   ls -lh /tmp/uapmd-wasm-build/build/
   ```
   Should see: `uapmd-app.js`, `uapmd-app.wasm`, `index.html`

3. **Canvas size issue**:
   - The canvas might be 0x0 pixels initially
   - ImGui should auto-resize it on first frame

4. **Check if SDL2 and WASM loaded**:
   - Open browser DevTools (F12)
   - Check Console for "SDL2" or "WASM" messages
   - Look for initialization errors

### Build still shows X11 errors

The old `build-standalone.sh` script may have leftover CMake cache. Use `build-clean.sh` instead:
- ✅ Uses isolated `/tmp/uapmd-wasm-build/` directory
- ✅ Creates fresh CMakeLists.txt each time
- ✅ No X11 or desktop dependencies
