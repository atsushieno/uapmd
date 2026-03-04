# Coding Guidelines for AI agents

## Primary development documentation

You have to follow our [Developers Guide](docs/DEVELOPERS.md) that is written for human beings.

## Build, Test, and Development Commands

- For desktop platforms:
  - Configure (Ninja on Linux/macOS): `cmake -B cmake-build-debug -G Ninja -DCPM_SOURCE_CACHE=~/.cache/CPM/uapmd`
  - Configure (Windows/MSVC): `cmake -B cmake-build-debug-visual-studio -G "Visual Studio 17 2022" -DBUILD_SHARED_LIBS=OFF  -DCPM_SOURCE_CACHE=%HOME%\.cache\CPM\uapmd` (`%HOME%` should be altered by the environment variable)
  - Build: `cmake --build (per-target/platform directory)`
  - Run: `cmake-build-debug/source/tools/uapmd-app/uapmd-app`
  - CI builds on Ubuntu, macOS, and Windows (see `.github/workflows/actions.yml`).
- For Emscripten: `bash build-wasm.sh CPM_SOURCE_CACHE=~/.cache/CPM/uapmd`
- For Android: `cd android && ./gradlew build`
- For iOS (macOS only): `bash build-ios-sim.sh --skip-configure --skip-launch CPM_SOURCE_CACHE=~/.cache/CPM/uapmd`
  - If you need CMake bootstrap then remove `--skip-configure`
  - If you need to install and launch then remove `--skip-launch`

**IMPORTANT** you are strictly prohibited to run the following build related commands without explicit permission from me:

- `cmake -B` (including further arguments)
- `rm -rf (per-target/platform directory)`
- `git clean -xd` (including further arguments)
- `rm -rf ~/.cache/CPM/uapmd`

## C++ Coding Style & Naming Conventions

- `if` - `else`, `for`, and `while`: unwrap single-line statements from `{`... `}`.
- Naming:
  - PascalCase for types/classes;
  - camelCase for methods/functions;
  - snake_case for fields (add `_` as suffix for conflicts);
  - ALL_CAPS and enums (excluding `enum class`-es) for macros;
  - C interop retains library naming.
- Headers: public `.hpp` in `include/`; keep private APIs under `src/*`.
- Prefer RAII, `std::` facilities
- You must follow lock‑free or non‑blocking patterns in audio thread, and if you cannot you have to report back.

## Scope of work

- Commits and PRs are made only by humans.
- We don't expect you to write tests much. When we need, we will ask explicitly.
