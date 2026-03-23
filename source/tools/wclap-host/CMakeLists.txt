# uapmd-wclap-host.wasm
#
# A standalone Emscripten WASM module that wclap.mjs loads directly (no
# Emscripten JS glue).  It provides five exported bridge functions that let
# wclap.mjs create and manage cross-WASM plugin instances.  All CLAP host
# interaction is handled on the JS side via the `_wclapInstance.*` imports
# that wclap.mjs satisfies at instantiation time.
#
# Only compiled as part of WASM (Emscripten) builds.

# Clear the global Emscripten linker flags inherited from the parent scope.
# uapmd-wclap-host is a *standalone* WASM reactor and needs its own minimal set of
# flags (STANDALONE_WASM + --no-entry).  The main-app flags (NO_EXIT_RUNTIME,
# AUDIO_WORKLET, EXPORTED_FUNCTIONS, …) are incompatible with STANDALONE_WASM.
# This assignment is scoped to this subdirectory and does not propagate back.
set(CMAKE_EXE_LINKER_FLAGS "")

# ── CPM: wclap-host-cpp ────────────────────────────────────────────────────────
# Header-only C++ library providing wclap32/wclap64 pointer/function types,
# wclap::Instance<> template, and CLAP type definitions.

CPMAddPackage(
    NAME wclap-host-cpp
    GIT_REPOSITORY https://github.com/WebCLAP/wclap-host-cpp
    GIT_TAG main
    DOWNLOAD_ONLY YES
)

# ── CPM: wclap-host-js ─────────────────────────────────────────────────────────
# Provides:
#   cpp/wclap-js-instance.h  — js_wasm::WclapInstance and the extern "C" bridge
#   wclap.mjs                — top-level JS re-export
#   es6/                     — actual JS runtime (wclap.mjs, wclap-plugin.mjs, …)
#   es6/wasi/                — WASI runtime that satisfies WASI imports in the WASM
#
# GIT_SUBMODULES initialises es6/wasi alongside the main checkout.

CPMAddPackage(
    NAME wclap-host-js
    GIT_REPOSITORY https://github.com/WebCLAP/wclap-host-js
    GIT_TAG main
    GIT_SUBMODULES "es6/wasi"
    DOWNLOAD_ONLY YES
)

# ── uapmd-wclap-host executable (produces uapmd-wclap-host.wasm) ──────────────

add_executable(uapmd-wclap-host WclapHost.cpp)
set_target_properties(uapmd-wclap-host PROPERTIES OUTPUT_NAME "uapmd-wclap-host")

target_include_directories(uapmd-wclap-host PRIVATE
    "${wclap-host-cpp_SOURCE_DIR}/include"   # wclap/instance.hpp, wclap/wclap.hpp
    "${wclap-host-js_SOURCE_DIR}/cpp"        # wclap-js-instance.h
)

target_compile_options(uapmd-wclap-host PRIVATE
    -fno-exceptions
    -fno-rtti
)

# Standalone WASM: no Emscripten JS glue wrapper.
# wclap.mjs instantiates the module directly and provides all imports:
#   - _wclapInstance.*  (the host bridge functions)
#   - wasi_snapshot_preview1.*  (via es6/wasi/ WASI runtime)
#
# Five functions are exported; _wclapNextThreadId and _wclapStartInstanceThread
# already carry __attribute__((export_name)) so the linker flags are technically
# redundant for those two, but listed for clarity.
target_link_options(uapmd-wclap-host PRIVATE
    "-sSTANDALONE_WASM=1"
    "--no-entry"
    "-Wl,--export-table"
    "-Wl,--export=__indirect_function_table"
    # _wclapInstance.* imports are satisfied at runtime by wclap.mjs.
    # wasi_snapshot_preview1.* imports are satisfied by es6/wasi/.
    # Both sets are intentional WASM imports, not missing link-time symbols.
    "-sERROR_ON_UNDEFINED_SYMBOLS=0"
    # Instance lifecycle
    "-Wl,--export=_wclapInstanceCreate"
    "-Wl,--export=_wclapInstanceSetPath"
    "-Wl,--export=_wclapInstanceGetNextIndex"
    "-Wl,--export=_wclapNextThreadId"
    "-Wl,--export=_wclapStartInstanceThread"
    # CLAP plugin lifecycle and audio processing
    "-Wl,--export=_wclapPluginSetup"
    "-Wl,--export=_wclapPluginStartProcessing"
    "-Wl,--export=_wclapPluginStopProcessing"
    "-Wl,--export=_wclapPluginProcess"
    "-Wl,--export=_wclapGetInputL"
    "-Wl,--export=_wclapGetInputR"
    "-Wl,--export=_wclapGetOutputL"
    "-Wl,--export=_wclapGetOutputR"
    "-Wl,--export=_wclapHostGetExt"
    "-Wl,--export=_wclapHostReqRestart"
    "-Wl,--export=_wclapHostReqProcess"
    "-Wl,--export=_wclapHostReqCallback"
    "-Wl,--export=_wclapInEventsSize"
    "-Wl,--export=_wclapInEventsGet"
    "-Wl,--export=_wclapOutEventsTryPush"
    "-Wl,--export=_wclapDescribeParameters"
    "-Wl,--export=_wclapEnqueueMidi2Event"
    "-Wl,--export=_wclapEnqueueParameterValue"
    "-Wl,--export=_wclapPluginDestroy"
)

# ── Deploy uapmd-wclap-host.wasm + wclap.mjs + es6/ to the web root ───────────

set(_WCLAP_WEB_DST "${CMAKE_CURRENT_BINARY_DIR}/../uapmd-app")

add_custom_command(TARGET uapmd-wclap-host POST_BUILD
    # uapmd-wclap-host.wasm — the built artifact
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/uapmd-wclap-host.wasm"
            "${_WCLAP_WEB_DST}/uapmd-wclap-host.wasm"
    # wclap.mjs — top-level re-export (import { getHost, startHost, getWclap })
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${wclap-host-js_SOURCE_DIR}/wclap.mjs"
            "${_WCLAP_WEB_DST}/wclap.mjs"
    # es6/ — full module tree including wasi/ runtime
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${wclap-host-js_SOURCE_DIR}/es6"
            "${_WCLAP_WEB_DST}/es6"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web-overrides/es6/wclap.mjs"
            "${_WCLAP_WEB_DST}/es6/wclap.mjs"
    COMMENT "Deploying uapmd-wclap-host.wasm, wclap.mjs, and es6/ to web build directory"
)
