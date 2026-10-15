#!/usr/bin/env bash
# Builds the JSFX benchmark (native JIT, native interpreter, wasm interpreter) and the EEL2 wasm spike.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../../.." && pwd)"
OUT="${REPO_ROOT}/cmake-build-eel2-wasm"
WASM_BUILD="${REPO_ROOT}/cmake-build-wasm"
EMSDK_INFO="${REPO_ROOT}/source/tools/uapmd-app/web/.emsdk-info"

if [[ ! -f "${WASM_BUILD}/CMakeCache.txt" ]]; then
    echo "error: ${WASM_BUILD} is not configured; run build-wasm.sh first" >&2
    exit 1
fi
# ysfx lives in the CPM source cache or in _deps depending on how the wasm build was configured.
YSFX_SRC="$(sed -n 's/^ysfx_SOURCE_DIR:STATIC=//p' "${WASM_BUILD}/CMakeCache.txt")"
YSFX_WASM_LIB="$(sed -n 's/^ysfx_BINARY_DIR:STATIC=//p' "${WASM_BUILD}/CMakeCache.txt")/libysfx.a"

if [[ ! -f "${YSFX_WASM_LIB}" ]]; then
    echo "error: ${YSFX_WASM_LIB} not found; run build-wasm.sh first" >&2
    exit 1
fi
# The wasm libraries must be linked by the same emsdk that built them (libc++ ABI).
# shellcheck disable=SC1090
source "${EMSDK_INFO}"
EMXX="${UAPMD_EMSDK_ROOT}/upstream/emscripten/em++"

mkdir -p "${OUT}"

build_native_bench() {
    local dir="$1"; shift
    local flags="-O3 -DNDEBUG -fsigned-char -DYSFX_NO_GFX -DWDL_FFT_REALSIZE=8 -DWDL_LINEPARSE_ATOF=ysfx_wdl_atof"
    flags+=" -I${YSFX_SRC}/include -I${YSFX_SRC}/sources -I${YSFX_SRC}/thirdparty/WDL/source -I${YSFX_SRC}/thirdparty/dr_libs $*"
    mkdir -p "${dir}"
    for f in nseel-caltab nseel-cfunc nseel-compiler nseel-eval nseel-lextab nseel-ram nseel-yylex; do
        clang -c ${flags} "${YSFX_SRC}/thirdparty/WDL/source/WDL/eel2/${f}.c" -o "${dir}/${f}.o" &
    done
    clang -c ${flags} "${YSFX_SRC}/thirdparty/WDL/source/WDL/fft.c" -o "${dir}/fft.o" &
    for f in ysfx ysfx_config ysfx_midi ysfx_reader ysfx_parse ysfx_parse_menu ysfx_preset ysfx_audio_wav \
             ysfx_audio_flac ysfx_utils ysfx_utils_fts ysfx_api_eel ysfx_api_reaper ysfx_api_file ysfx_api_gfx \
             ysfx_eel_utils ysfx_preprocess; do
        clang++ -std=c++17 -c ${flags} "${YSFX_SRC}/sources/${f}.cpp" -o "${dir}/${f}.o" &
    done
    wait
    clang++ -std=c++17 -O3 -I"${YSFX_SRC}/include" "${HERE}/bench/jsfx-bench.cpp" "${dir}"/*.o -o "${dir}/jsfx-bench"
}

echo "native JIT benchmark..."
build_native_bench "${OUT}/native-jit"
echo "native interpreter benchmark..."
build_native_bench "${OUT}/native-portable" -DEEL_TARGET_PORTABLE

echo "wasm interpreter benchmark..."
mkdir -p "${OUT}/wasm-portable"
"${EMXX}" -O3 -std=c++17 -I"${YSFX_SRC}/include" "${HERE}/bench/jsfx-bench.cpp" "${YSFX_WASM_LIB}" \
    -pthread -sNODERAWFS=1 -sINITIAL_MEMORY=256MB -sERROR_ON_UNDEFINED_SYMBOLS=0 -sSTACK_SIZE=8MB \
    -o "${OUT}/wasm-portable/jsfx-bench.js"

echo "wasm backend (patched ysfx from source)..."
PATCHED="${OUT}/ysfx-patched"
rm -rf "${PATCHED}"
mkdir -p "${PATCHED}/thirdparty/WDL/source" "${OUT}/wasm-backend"
cp -R "${YSFX_SRC}/include" "${YSFX_SRC}/sources" "${PATCHED}/"
cp -R "${YSFX_SRC}/thirdparty/dr_libs" "${PATCHED}/thirdparty/"
cp -R "${YSFX_SRC}/thirdparty/WDL/source/WDL" "${PATCHED}/thirdparty/WDL/source/"
# the wasm build's own ysfx already carries the patch once uapmd-format-jsfx applies it
if ! grep -q NSEEL_WASM_BACKEND "${PATCHED}/thirdparty/WDL/source/WDL/eel2/nseel-compiler.c"; then
    (cd "${PATCHED}" && /usr/bin/patch -p1 --forward --batch < "${REPO_ROOT}/external/patches/ysfx-eel2-wasm-backend.patch" > /dev/null)
fi
EMCC="${UAPMD_EMSDK_ROOT}/upstream/emscripten/emcc"
WFLAGS="-O3 -DNDEBUG -fsigned-char -pthread -DEEL_TARGET_PORTABLE -DYSFX_NO_GFX -DYSFX_NO_FTS -DNSEEL_WASM_BACKEND -DNSEEL_WASM_DEBUG"
WFLAGS+=" -DWDL_FFT_REALSIZE=8 -DWDL_LINEPARSE_ATOF=ysfx_wdl_atof -Wno-unused-function -Wno-unused-variable"
WFLAGS+=" -I${PATCHED}/include -I${PATCHED}/sources -I${PATCHED}/thirdparty/WDL/source -I${PATCHED}/thirdparty/dr_libs"
for f in nseel-caltab nseel-cfunc nseel-compiler nseel-eval nseel-lextab nseel-ram nseel-yylex; do
    "${EMCC}" -c ${WFLAGS} "${PATCHED}/thirdparty/WDL/source/WDL/eel2/${f}.c" -o "${OUT}/wasm-backend/${f}.o" &
done
"${EMCC}" -c ${WFLAGS} "${PATCHED}/thirdparty/WDL/source/WDL/fft.c" -o "${OUT}/wasm-backend/fft.o" &
"${EMCC}" -c ${WFLAGS} "${HERE}/nseel-wasm.c" -o "${OUT}/wasm-backend/nseel-wasm.o" &
for f in ysfx ysfx_config ysfx_midi ysfx_reader ysfx_parse ysfx_parse_menu ysfx_preset ysfx_audio_wav \
         ysfx_audio_flac ysfx_utils ysfx_utils_fts ysfx_api_eel ysfx_api_reaper ysfx_api_file ysfx_api_gfx \
         ysfx_eel_utils ysfx_preprocess; do
    "${EMXX}" -std=c++17 -c ${WFLAGS} "${PATCHED}/sources/${f}.cpp" -o "${OUT}/wasm-backend/${f}.o" &
done
wait
"${EMXX}" -std=c++17 ${WFLAGS} "${HERE}/bench/jsfx-wasm-check.cpp" "${OUT}"/wasm-backend/*.o \
    -sNODERAWFS=1 -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH=1 -sSTACK_SIZE=8MB --profiling-funcs \
    -o "${OUT}/wasm-backend/jsfx-wasm-check.js"

echo "wasm backend spike..."
mkdir -p "${OUT}/spike"
"${EMXX}" -O3 -std=c++17 "${HERE}/spike/eel2-wasm-spike.cpp" \
    -pthread -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH=1 -sPTHREAD_POOL_SIZE=2 -sEXIT_RUNTIME=1 \
    -o "${OUT}/spike/eel2-wasm-spike.js"

echo "Built into ${OUT}"
