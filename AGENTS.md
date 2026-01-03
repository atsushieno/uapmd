# Coding Guidelines for AI agents

## Primary development documentation

You have to follow our [Developers Guide](docs/DEVELOPERS.md) that is written for human beings.

## Build, Test, and Development Commands

- Configure (Ninja on Linux/macOS): `cmake -B cmake-build-debug -G Ninja`
- Configure (Windows/MSVC): `cmake -B build -G "Visual Studio 17 2022" -DBUILD_SHARED_LIBS=OFF`
- Build: `cmake --build cmake-build-debug`
- Example runs (after build):
  - Scan plugins: `cmake-build-debug/tools/remidy-scan/remidy-scan`
  - Start service: `cmake-build-debug/tools/uapmd-app/uapmd-app`
- CI builds on Ubuntu, macOS, and Windows (see `.github/workflows/actions.yml`).

## Coding Style & Naming Conventions

- Language: C++23, CMake for builds.
- Indentation: 4 spaces; braces on same line for functions/classes.
- C/C++ `if` - `else`, `for`, and `while`: unwrap single-line statements from `{`... `}`.
- Naming:
  - PascalCase for types/classes;
  - camelCase for methods/functions;
  - snake_case for fields (add `_` as suffix for conflicts);
  - ALL_CAPS for macros;
  - C interop retains library naming.
- Headers: public `.hpp` in `include/`; keep private APIs under `src/*`.
- Prefer RAII, `std::` facilities, and lock‑free or non‑blocking patterns in audio paths.

## Testing Guidelines

- No dedicated unit tests yet; CI ensures cross‑platform builds.
- Add small, focused tests alongside modules using CTest/GoogleTest when practical.
- Use smoke tests: run `remidy-scan`, then launch `uapmd-app` with a known plugin and verify audio/MIDI routing.

## Commit & Pull Request Guidelines

- Commits: code is committed only by humans.
  - If drafted: keep concise, imperative mood (e.g., "Fix VST3 parameter mapping"). Group related changes; avoid noisy reformatting.
- PRs: PRs are only made, updated, and resolved only by humans.
  - If drafted: include a clear summary, reproduction steps, platform notes (Linux/macOS/Windows), and linked issues. Ensure `cmake --build cmake-build-debug` succeeds on all OSes.
- Keep changes modular: public API headers in `include/`, library sources and non-public headers in `src/`, tools sources in `tools/`, and update `CMakeLists.txt` as needed.

## Security & Configuration Tips

- Plugin scanning/hosting loads third‑party code. Test new plugins in a clean environment. Do not commit proprietary SDKs or local plugin paths.
- Supported formats: VST3, AU (macOS), LV2, CLAP.
