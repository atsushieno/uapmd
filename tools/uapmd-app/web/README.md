# uapmd WebAssembly Target

This directory hosts the experimental WebAssembly build for `uapmd-app`. The goal is to reuse the existing ImGui+SDL2 desktop UI and compile it with Emscripten so that the exact same interface can run inside a browser canvas.

> **Status**: the build scripts and CMake glue are in place; functionality parity still depends on how quickly we can pull changes from `main` into the wasm runtime.

## Quick Start

```bash
./build-wasm-imgui.sh
```

The top-level script will:

1. Fetch a local copy of `emsdk` (stored under `tools/uapmd-app/web/.emsdk`) if it has not been downloaded yet.
2. Install/activate the requested Emscripten SDK version inside `tools/uapmd-app/web/.emsdk`.
3. Configure the **root** CMakeLists with `emcmake` and `-DUAPMD_TARGET_WASM=ON`.
4. Emit `uapmd-app.html/.js/.wasm` inside `cmake-build-wasm/`.

After the build you can run a dev server from the web assets directory:

```bash
cd tools/uapmd-app/web
npm install
npm run serve
```

Visit http://localhost:8080 to load the UI.

Prefer to drive CMake manually? Use:

```bash
emcmake cmake -S . -B cmake-build-wasm -G Ninja -DUAPMD_TARGET_WASM=ON -DUAPMD_BUILD_TESTS=OFF
cmake --build cmake-build-wasm --target uapmd-app
```

## File Tour

| Path | Description |
| --- | --- |
| `package.json` | Scripts for hosting the Wasm artifacts via a static server. |
| `../uapmd_wasm.cpp` / `../uapmd_wasm_api.h` | Minimal C ABI shim used by `build-wasm.sh` to exercise the JS bridge. |
| `README.md` | This document. |
| `../web_main*.cpp` | Browser entry points that live next to other platform sources. |
| `../../../build-wasm-imgui.sh` | Root-level orchestration script that drives the build. |
| `../../../build-wasm.sh` | Minimal stub builder for experimenting with the lightweight FFI surface. |

## Controlling the SDK Version

The helper script defaults to `latest`. Override with environment variables:

```bash
UAPMD_EMSDK_VERSION=3.1.62 ./build-wasm-imgui.sh
```

You can also skip the automatic checkout and use an existing emsdk install:

```bash
./build-wasm-imgui.sh --use-system
```

Set `UAPMD_USE_SYSTEM_EMSDK=true ./build-wasm.sh` if you want the stub builder to do the same.

## Cleanup

Generated artifacts live under `cmake-build-wasm/` (for the full build) and `tools/uapmd-app/web/dist/` (for the stub). The private SDK cache lives under `tools/uapmd-app/web/.emsdk`. Delete them manually if you need to reclaim space.

## Troubleshooting

- **Build fails before configuration**: check that `python3` is installed; the fetch script uses it to run `emsdk.py`.
- **`emcmake` still missing**: delete `tools/uapmd-app/web/.emsdk` and rerun the script to force a clean install.
- **White screen in the browser**: open DevTools and inspect console errors; most issues stem from missing audio/MIDI permissions.

The wasm layout mirrors the original `wasm` branch contents while keeping everything scoped under `tools/uapmd-app`, so we can iterate without polluting the repo root.
