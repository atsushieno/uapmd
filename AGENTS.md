# Repository Guidelines

## Project Structure & Module Organization
- `src/`: Core sources.
  - `src/remidy/`: Low-level plugin API abstraction (VST3/AU/LV2/CLAP backends).
  - `src/remidy-tooling/`: Scanning/instancing utilities for tools.
  - `src/uapmd/`: Virtual MIDI 2.0 device foundation (AudioBackend, AudioPluginHosting, Sequencer).
- `include/`: Public headers (`include/<module>`), internals under `include/<module>/priv`.
- `tools/`: CLI tools (e.g., `uapmd-service`, `remidy-scan`, `remidy-plugin-host`).
- `external/`: Third‑party dependencies (submodules/FetchContent).
- `cmake/`: Build helpers; `CMakeLists.txt` orchestrates all modules.

## Build, Test, and Development Commands
- Configure (Ninja on Linux/macOS): `cmake -B cmake-build-debug -G Ninja`
- Configure (Windows/MSVC): `cmake -B build -G "Visual Studio 17 2022" -DBUILD_SHARED_LIBS=OFF`
- Build: `cmake --build cmake-build-debug`
- Example runs (after build):
  - Scan plugins: `cmake-build-debug/tools/remidy-scan/remidy-scan`
  - Start service: `cmake-build-debug/tools/uapmd-service/uapmd-service "MySynth" VST3 PIPEWIRE`
- CI builds on Ubuntu, macOS, and Windows (see `.github/workflows/actions.yml`).

## Coding Style & Naming Conventions
- Language: C++23, CMake for builds.
- Indentation: 4 spaces; braces on same line for functions/classes.
- Naming: PascalCase for types/classes; camelCase for methods/functions; ALL_CAPS for macros; C interop retains library naming.
- Headers: public `.hpp` in `include/`; keep private APIs under `include/*/priv`.
- Prefer RAII, `std::` facilities, and lock‑free or non‑blocking patterns in audio paths.

## Testing Guidelines
- No dedicated unit tests yet; CI ensures cross‑platform builds.
- Add small, focused tests alongside modules using CTest/GoogleTest when practical.
- Use smoke tests: run `remidy-scan`, then launch `uapmd-service` with a known plugin and verify audio/MIDI routing.

## Commit & Pull Request Guidelines
- Commits: concise, imperative mood (e.g., "Fix VST3 parameter mapping"). Group related changes; avoid noisy reformatting.
- PRs: include a clear summary, reproduction steps, platform notes (Linux/macOS/Windows), and linked issues. Ensure `cmake --build cmake-build-debug` succeeds on all OSes.
- Keep changes modular: headers in `include/`, sources in `src/`, tools in `tools/`, and update `CMakeLists.txt` as needed.

## Security & Configuration Tips
- Plugin scanning/hosting loads third‑party code. Test new plugins in a clean environment. Do not commit proprietary SDKs or local plugin paths.
- Supported formats: VST3, AU (macOS), LV2, CLAP. Some backends require platform packages (see CI for hints).

