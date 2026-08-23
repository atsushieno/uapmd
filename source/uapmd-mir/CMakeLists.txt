# This directory holds several independent features.
#
# The uapmd-mir addin library itself is always built, because it always carries
# the pitch transcription addin. UAPMD_ENABLE_MIR gates only the music-analysis
# addins inside it -- their results are not yet trustworthy -- so librosa.cpp
# and libsonare are not fetched at all for builds that do not want the analysis
# commands. Pitch transcription needs neither of them. The stem separation
# addins below are always built too, as separate static built-in libraries.
#
# The library is a dynamically loaded addin, so it is built only where the addin
# manager can load one; WebAssembly, Android and iOS use built-in addins only.
if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT IOS)

# Header-only, and pinned to the same commit librosa.cpp vendors, so an
# MIR-enabled build compiles one identical copy of pocketfft either way. Fetched
# outside the analysis gate because the pitch estimators always need it.
CPMAddPackage(
        NAME           pocketfft
        GIT_REPOSITORY https://github.com/mreineck/pocketfft
        GIT_TAG        32424d2067c2e8043dc646a4e49754b2b40cc549
        DOWNLOAD_ONLY  YES
)

# MPM and YIN, taken as source rather than as a CMake dependency. DOWNLOAD_ONLY
# because upstream's own CMakeLists cannot be used: its public header pulls in
# mlpack 3 (which the plain estimators do not need but every estimator inherits)
# and FFTS, and it sets -march=native -fext-numeric-literals -ansi globally,
# which neither configures under AppleClang nor survives a cross-compile.
#
# Only src/mpm.cpp and src/yin.cpp are compiled, and they are compiled
# unmodified: src/pitch-detection/pitch_detection.h replaces upstream's public
# header, so the mlpack and FFTS members disappear, and the FFT, the bounds fix
# in parabolic interpolation and the HMM stub live beside it. Our include
# directory must precede upstream's for that substitution to take effect --
# which is why ${pitch_detection_SOURCE_DIR}/include is deliberately absent
# below. See src/pitch-detection/HACKING.md.
CPMAddPackage(
        NAME           pitch_detection
        GIT_REPOSITORY https://github.com/sevagh/pitch-detection
        GIT_TAG        8055f50bd94ffcc9fec4b63ca9a6a087eec9d892
        DOWNLOAD_ONLY  YES
)

if(UAPMD_ENABLE_MIR)

    set(_UAPMD_MIR_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)

    if(UAPMD_ENABLE_LIBSONARE AND NOT WIN32 AND NOT IOS AND NOT EMSCRIPTEN)
        CPMAddPackage(
                NAME           libsonare
                GIT_TAG        develop
                GIT_REPOSITORY https://github.com/libraz/libsonare
                EXCLUDE_FROM_ALL
                OPTIONS
                    "BUILD_TESTING OFF"
                    "BUILD_CLI OFF"
                    "BUILD_BENCH OFF"
                    "BUILD_MASTERING OFF"
                    "BUILD_MIXING OFF"
                    "BUILD_GRAPH OFF"
                    "BUILD_FX OFF"
                    "BUILD_ACOUSTIC_SIM OFF"
                    "BUILD_ARRANGEMENT OFF"
                    "BUILD_ASSIST OFF"
                    "SONARE_WITH_FFMPEG OFF"
        )
        # libsonare v1.7.2 omits this source from its analysis target.
        if(TARGET sonare_core_objects)
            target_sources(sonare_core_objects PRIVATE
                    "${libsonare_SOURCE_DIR}/src/rt/biquad_design.cpp"
            )
        endif()
    endif()

    # librosa.cpp backs the always-available backend of this module, so it is
    # mandatory here -- but the module itself is opt-in, so nothing is fetched
    # unless UAPMD_ENABLE_MIR is set. Added after libsonare so both libraries
    # reuse the same Eigen target when libsonare is enabled.
    CPMAddPackage(
            NAME           librosa_cpp
            GIT_TAG        ce1db117fcc75d401c42a9ade63f4406777c2660
            GIT_REPOSITORY https://github.com/olilarkin/librosa.cpp
            GIT_SUBMODULES "modules/eigen;modules/pocketfft"
            EXCLUDE_FROM_ALL
            OPTIONS
                "LIBROSA_BUILD_TESTS OFF"
                "LIBROSA_BUILD_EXAMPLES OFF"
                "LIBROSA_BUILD_BENCHMARKS OFF"
                "LIBROSA_BUILD_CROSSVAL_TESTS OFF"
                "LIBROSA_BUILD_CLI OFF"
                "LIBROSA_BUILD_WASM OFF"
                "LIBROSA_BUILD_SWIFT_C_WRAPPER OFF"
                "LIBROSA_FFT_BACKEND pocketfft"
    )

    if(DEFINED _UAPMD_MIR_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_MIR_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
        set(BUILD_SHARED_LIBS "${_UAPMD_MIR_OLD_BUILD_SHARED_LIBS}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()

endif()

# The library, and the pitch transcription addin in it, exist regardless of
# UAPMD_ENABLE_MIR. AddinEntry.cpp assembles whichever addins were compiled in.
add_library(uapmd-mir SHARED
        src/AddinEntry.cpp
        src/PitchTranscriptionAddin.cpp
        src/PitchTranscription.cpp
        src/NoteClipWriter.cpp
        src/ClipTranscription.cpp
        src/pitch-detection/autocorrelation.cpp
        src/pitch-detection/parabolic_interpolation.cpp
        src/pitch-detection/HmmStub.cpp
        "${pitch_detection_SOURCE_DIR}/src/mpm.cpp"
        "${pitch_detection_SOURCE_DIR}/src/yin.cpp"
)

add_uapmd_addin_library(uapmd-mir)

target_compile_features(uapmd-mir PRIVATE cxx_std_23)

target_include_directories(uapmd-mir PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pitch-detection
        ${pocketfft_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-engine/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-graph/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
        ${midicci_SOURCE_DIR}/include
)
target_link_libraries(uapmd-mir PRIVATE
        uapmd-engine
        uapmd-data
)

# Transcription runs an FFT per analysis frame over a whole clip, which at the
# Debug default of -O0 takes far longer than the algorithms need.
uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-mir
        src/PitchTranscription.cpp
        src/pitch-detection/autocorrelation.cpp
        "${pitch_detection_SOURCE_DIR}/src/mpm.cpp"
        "${pitch_detection_SOURCE_DIR}/src/yin.cpp"
)

if(UAPMD_ENABLE_MIR)

    target_sources(uapmd-mir PRIVATE
            src/MirAddin.cpp
            src/MirLibrosaAddin.cpp
            src/MirLibrosaAnalysis.cpp
            src/MirTempoMap.cpp
            src/MirMeterMap.cpp
    )

    target_compile_definitions(uapmd-mir PRIVATE
            UAPMD_ENABLE_MIR_ANALYSIS=1
            UAPMD_ENABLE_LIBROSA=1
    )

    target_link_libraries(uapmd-mir PRIVATE librosa::librosa)

    if(UAPMD_ENABLE_LIBSONARE AND TARGET sonare_core)
        target_compile_definitions(uapmd-mir PRIVATE UAPMD_ENABLE_LIBSONARE=1)
    else()
        target_compile_definitions(uapmd-mir PRIVATE UAPMD_ENABLE_LIBSONARE=0)
    endif()

    if(UAPMD_ENABLE_LIBSONARE AND TARGET sonare_core)
        target_sources(uapmd-mir PRIVATE
                src/MirRhythmAnalysis.cpp
                src/MirTempoAnalysis.cpp
        )
        target_include_directories(uapmd-mir PRIVATE
                ${libsonare_SOURCE_DIR}/include
        )
        target_link_libraries(uapmd-mir PRIVATE
                sonare_core
        )

        if(UAPMD_BUILD_TESTS AND NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
            add_executable(uapmd-mir-tempo-probe
                    tests/TempoMapProbe.cpp
                    src/MirRhythmAnalysis.cpp
                    src/MirTempoAnalysis.cpp
                    src/MirLibrosaAnalysis.cpp
                    src/MirTempoMap.cpp
                    src/MirMeterMap.cpp
            )
            target_compile_features(uapmd-mir-tempo-probe PRIVATE cxx_std_23)
            target_compile_definitions(uapmd-mir-tempo-probe PRIVATE UAPMD_ENABLE_LIBSONARE=1)
            target_include_directories(uapmd-mir-tempo-probe PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/src
                    ${libsonare_SOURCE_DIR}/include
            )
            target_link_libraries(uapmd-mir-tempo-probe PRIVATE sonare_core librosa::librosa)
        endif()
    endif()

else()

    target_compile_definitions(uapmd-mir PRIVATE
            UAPMD_ENABLE_MIR_ANALYSIS=0
            UAPMD_ENABLE_LIBROSA=0
            UAPMD_ENABLE_LIBSONARE=0
    )

endif()

if(APPLE)
    deploy_uapmd_addin_library(uapmd-mir
            "$<TARGET_BUNDLE_DIR:uapmd-app-bundle>/../addins")
endif()

endif() # NOT EMSCRIPTEN AND NOT ANDROID AND NOT IOS

# ---------------------------------------------------------------------------
# Shared stem separation support
#
# Reading, downmixing, resampling and writing stems is identical for every
# separation backend. It lives in its own target rather than in one of the
# addins because both addin libraries are linked into the same executable,
# and compiling it into each of them would collide at link time.
# ---------------------------------------------------------------------------

add_library(uapmd-stem-support STATIC
        src/StemSeparationSupport.cpp
)

target_compile_features(uapmd-stem-support PRIVATE cxx_std_23)

target_include_directories(uapmd-stem-support
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-graph/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-stem-support PRIVATE uapmd-data)

uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-stem-support
        src/StemSeparationSupport.cpp
)

# ---------------------------------------------------------------------------
# Demucs stem separation
#
# Built unconditionally, and as a *built-in* addin: uapmd-demucs is a static
# library that the application links WHOLE_ARCHIVE, so it needs no addin
# directory, no install rule of its own, and works the same on WebAssembly,
# Android and iOS, where dynamic addin loading is unavailable or has nowhere
# to scan.
# ---------------------------------------------------------------------------

CPMAddPackage(
    NAME demucscpp
    GIT_REPOSITORY https://github.com/sevagh/demucs.cpp
    GIT_TAG f1206e9adeea103aef4a636b9e62297cf1f8e34e
    DOWNLOAD_ONLY YES
)

set(_UAPMD_DEMUCS_PRIVATE_LIBS)
set(_UAPMD_DEMUCS_PRIVATE_DEFINES)
set(_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS)

if(UAPMD_DEMUCS_USE_OPENBLAS)
    if(APPLE)
        find_library(_UAPMD_ACCELERATE_FRAMEWORK Accelerate)
        if(_UAPMD_ACCELERATE_FRAMEWORK)
            list(APPEND _UAPMD_DEMUCS_PRIVATE_LIBS ${_UAPMD_ACCELERATE_FRAMEWORK})
            list(APPEND _UAPMD_DEMUCS_PRIVATE_DEFINES EIGEN_USE_BLAS)
        else()
            message(WARNING "Accelerate framework not found on this Apple toolchain; Demucs BLAS acceleration disabled.")
        endif()
    else()
        set(_UAPMD_OPENBLAS_DYNAMIC_ARCH ON)
        set(_UAPMD_OPENBLAS_TARGET "")
        if(CMAKE_CROSSCOMPILING)
            set(_UAPMD_OPENBLAS_DYNAMIC_ARCH OFF)
            string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _uapmd_proc)
            if(_uapmd_proc MATCHES "aarch64" OR _uapmd_proc MATCHES "arm64")
                set(_UAPMD_OPENBLAS_TARGET ARMV8)
            elseif(_uapmd_proc MATCHES "armv7")
                set(_UAPMD_OPENBLAS_TARGET ARMV7)
            elseif(_uapmd_proc MATCHES "x86_64")
                set(_UAPMD_OPENBLAS_TARGET HASWELL)
            elseif(_uapmd_proc MATCHES "i[3-6]86" OR _uapmd_proc MATCHES "x86")
                set(_UAPMD_OPENBLAS_TARGET PENTIUM4)
            else()
                set(_UAPMD_OPENBLAS_TARGET GENERIC)
            endif()
        endif()
        set(_UAPMD_OPENBLAS_OPTIONS
                "BUILD_SHARED_LIBS OFF"
                "BUILD_TESTING OFF"
                "BUILD_WITHOUT_LAPACK ON"
                "DYNAMIC_ARCH ${_UAPMD_OPENBLAS_DYNAMIC_ARCH}"
                "NOFORTRAN ON"
                "CMAKE_POLICY_VERSION 3.5"
                "CMAKE_POLICY_VERSION_MINIMUM 3.5"
        )
        if(_UAPMD_OPENBLAS_TARGET)
            list(APPEND _UAPMD_OPENBLAS_OPTIONS "TARGET ${_UAPMD_OPENBLAS_TARGET}")
        endif()
        CPMAddPackage(
                NAME openblas
                GIT_REPOSITORY https://github.com/OpenMathLib/OpenBLAS
                GIT_TAG v0.3.26
                EXCLUDE_FROM_ALL YES
                OPTIONS ${_UAPMD_OPENBLAS_OPTIONS}
        )
        if(NOT TARGET openblas)
            message(FATAL_ERROR "Requested UAPMD_DEMUCS_USE_OPENBLAS=ON, but the OpenBLAS target was not created.")
        endif()
        list(APPEND _UAPMD_DEMUCS_PRIVATE_LIBS openblas)
        list(APPEND _UAPMD_DEMUCS_PRIVATE_DEFINES EIGEN_USE_BLAS)
        list(APPEND _UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS
                ${openblas_SOURCE_DIR}
                ${openblas_SOURCE_DIR}/include
                ${openblas_SOURCE_DIR}/interface
                ${openblas_BINARY_DIR}
                ${openblas_BINARY_DIR}/generated
        )
    endif()
endif()

if(NOT TARGET demucs-cpp)
    file(GLOB DEMUCSPP_SOURCES
        ${demucscpp_SOURCE_DIR}/src/*.cpp
    )
    add_library(demucs-cpp STATIC ${DEMUCSPP_SOURCES})
    target_include_directories(demucs-cpp PUBLIC
        ${demucscpp_SOURCE_DIR}/src
        ${demucscpp_SOURCE_DIR}/vendor/eigen
    )
    target_compile_features(demucs-cpp PUBLIC cxx_std_17)
    if(_UAPMD_DEMUCS_PRIVATE_LIBS)
        target_link_libraries(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_LIBS})
    endif()
    if(_UAPMD_DEMUCS_PRIVATE_DEFINES)
        target_compile_definitions(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_DEFINES})
    endif()
    if(_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS)
        target_include_directories(demucs-cpp PRIVATE ${_UAPMD_DEMUCS_PRIVATE_INCLUDE_DIRS})
    endif()
    uapmd_optimize_heavy_audio_feature_target_in_debug(demucs-cpp)
endif()

# Not add_uapmd_addin_library(): that helper is for libraries the addin
# manager loads from disk, and this one is linked into the application.
add_library(uapmd-demucs STATIC
        src/DemucsAddin.cpp
        src/DemucsStemSeparator.cpp
)
add_library(uapmd::demucs ALIAS uapmd-demucs)

target_compile_features(uapmd-demucs PRIVATE cxx_std_23)

target_include_directories(uapmd-demucs PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-graph/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-demucs PRIVATE
        uapmd::uapmd-addin-core
        uapmd-stem-support
        uapmd-data
        demucs-cpp
)

uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-demucs
        src/DemucsStemSeparator.cpp
)

# ---------------------------------------------------------------------------
# BS-Roformer stem separation
#
# Built unconditionally and as a built-in addin, exactly like Demucs above.
# BSRoformer.cpp runs on GGML, which it resolves from a parent-provided `ggml`
# target when one exists -- so ggml is added first, CPU-only: no GPU backend
# needs its shader library or toolkit found at runtime, and every platform
# gets the same configuration.
# ---------------------------------------------------------------------------

set(_UAPMD_BSR_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

CPMAddPackage(
        NAME           ggml
        GIT_TAG        v0.9.11
        GIT_REPOSITORY https://github.com/ggml-org/ggml
        EXCLUDE_FROM_ALL
        OPTIONS
            "BUILD_SHARED_LIBS OFF"
            "GGML_BUILD_TESTS OFF"
            "GGML_BUILD_EXAMPLES OFF"
            "GGML_CUDA OFF"
            "GGML_METAL OFF"
            "GGML_VULKAN OFF"
            "GGML_OPENCL OFF"
            "GGML_HIP OFF"
            "GGML_SYCL OFF"
)

CPMAddPackage(
        NAME           bsroformer
        GIT_TAG        a7b9625f0f4146cacf3c46080d1139833cd4d4c2
        GIT_REPOSITORY https://github.com/chenmozhijin/BSRoformer.cpp
        EXCLUDE_FROM_ALL
        OPTIONS
            "GGML_CUDA OFF"
            "BSR_BUILD_CLI OFF"
            "BSR_BUILD_TESTS OFF"
)

if(DEFINED _UAPMD_BSR_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_BSR_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_UAPMD_BSR_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

# ggml's kernels *are* the compute path for BS-Roformer -- bs_roformer itself
# only builds the graph -- so leaving ggml at the Debug default of -O0 makes a
# separation run take orders of magnitude longer than the model needs. Give
# every ggml target the same Debug treatment demucs-cpp gets.
foreach(_uapmd_bsr_target bs_roformer ggml ggml-base ggml-cpu ggml-blas)
    if(NOT TARGET ${_uapmd_bsr_target})
        continue()
    endif()
    get_target_property(_uapmd_bsr_target_type ${_uapmd_bsr_target} TYPE)
    if(_uapmd_bsr_target_type STREQUAL "INTERFACE_LIBRARY")
        continue()
    endif()
    uapmd_optimize_heavy_audio_feature_target_in_debug(${_uapmd_bsr_target})
endforeach()

add_library(uapmd-bsroformer STATIC
        src/BSRoformerAddin.cpp
        src/BSRoformerStemSeparator.cpp
)
add_library(uapmd::bsroformer ALIAS uapmd-bsroformer)

target_compile_features(uapmd-bsroformer PRIVATE cxx_std_23)

target_include_directories(uapmd-bsroformer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-graph/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-bsroformer PRIVATE
        uapmd::uapmd-addin-core
        uapmd-stem-support
        uapmd-data
        bs_roformer
)

uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-bsroformer
        src/BSRoformerStemSeparator.cpp
)

# ---------------------------------------------------------------------------
# Basic Pitch polyphonic transcription
#
# Opt-in, and unlike everything else here the option guards a *download*: the
# model is Apache-2.0 and is not committed to this repository, so an enabled
# build fetches nmp.onnx at configure time and embeds it into the binary.
# Embedding rather than shipping a data file is what lets this work on
# WebAssembly and Android, where there is no sensible place to read one from.
#
# Built as a static built-in addin like the stem separators, and with no
# platform guard, because reaching those two targets is the whole reason this
# backend exists.
# ---------------------------------------------------------------------------

if(UAPMD_ENABLE_BASIC_PITCH)

    set(_UAPMD_BP_MODEL_URL
            "https://raw.githubusercontent.com/spotify/basic-pitch/main/basic_pitch/saved_models/icassp_2022/nmp.onnx")
    set(_UAPMD_BP_MODEL_SHA256
            "2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec")
    set(_UAPMD_BP_MODEL "${CMAKE_CURRENT_BINARY_DIR}/nmp.onnx")

    # The hash is what pins the model: `main` is a moving branch, so a silent
    # upstream retrain has to fail the build rather than change what ships.
    if(NOT EXISTS "${_UAPMD_BP_MODEL}")
        message(STATUS "Downloading the Basic Pitch model (Apache-2.0) from ${_UAPMD_BP_MODEL_URL}")
        file(DOWNLOAD "${_UAPMD_BP_MODEL_URL}" "${_UAPMD_BP_MODEL}"
                EXPECTED_HASH SHA256=${_UAPMD_BP_MODEL_SHA256}
                TLS_VERIFY ON
                STATUS _uapmd_bp_status)
        list(GET _uapmd_bp_status 0 _uapmd_bp_code)
        if(NOT _uapmd_bp_code EQUAL 0)
            list(GET _uapmd_bp_status 1 _uapmd_bp_message)
            file(REMOVE "${_UAPMD_BP_MODEL}")
            message(FATAL_ERROR
                    "Failed to download the Basic Pitch model: ${_uapmd_bp_message}. "
                    "Set UAPMD_ENABLE_BASIC_PITCH=OFF to build without it.")
        endif()
    endif()

    # Embedded as hex text in bounded chunks rather than as one byte array:
    # MSVC caps a string literal at 65535 bytes, and generating chunks with
    # string(SUBSTRING) is far quicker at configure time than a regex over
    # half a megabyte of hex.
    file(READ "${_UAPMD_BP_MODEL}" _uapmd_bp_hex HEX)
    string(LENGTH "${_uapmd_bp_hex}" _uapmd_bp_hex_length)
    math(EXPR _uapmd_bp_byte_count "${_uapmd_bp_hex_length} / 2")

    # Built with string(APPEND) on quoted arguments throughout. An unquoted
    # ${} expansion here would re-split the text on its semicolons and silently
    # strip every statement terminator from the generated C++.
    set(_uapmd_bp_generated "// GENERATED FILE -- do not edit, do not commit.\n")
    string(APPEND _uapmd_bp_generated "//\n")
    string(APPEND _uapmd_bp_generated "// Produced at configure time from nmp.onnx, downloaded from\n")
    string(APPEND _uapmd_bp_generated "//     ${_UAPMD_BP_MODEL_URL}\n")
    string(APPEND _uapmd_bp_generated "// pinned to SHA-256 ${_UAPMD_BP_MODEL_SHA256}.\n")
    string(APPEND _uapmd_bp_generated "//\n")
    string(APPEND _uapmd_bp_generated "// The bytes below are that file verbatim, hex encoded. They are the Basic\n")
    string(APPEND _uapmd_bp_generated "// Pitch model weights:\n")
    string(APPEND _uapmd_bp_generated "//\n")
    string(APPEND _uapmd_bp_generated "//     Basic Pitch, Copyright 2022 Spotify AB\n")
    string(APPEND _uapmd_bp_generated "//     Licensed under the Apache License, Version 2.0\n")
    string(APPEND _uapmd_bp_generated "//     http://www.apache.org/licenses/LICENSE-2.0\n")
    string(APPEND _uapmd_bp_generated "//\n")
    string(APPEND _uapmd_bp_generated "// Any binary built from this file redistributes those weights, so the\n")
    string(APPEND _uapmd_bp_generated "// LICENSE and NOTICE in src/basic-pitch/ must accompany it.\n")
    string(APPEND _uapmd_bp_generated "#include <cstddef>\n\n")
    string(APPEND _uapmd_bp_generated "extern const char* const kBasicPitchModelHexChunks[];\n")
    string(APPEND _uapmd_bp_generated "extern const size_t kBasicPitchModelHexChunkCount;\n")
    string(APPEND _uapmd_bp_generated "extern const size_t kBasicPitchModelByteCount;\n\n")
    string(APPEND _uapmd_bp_generated "extern const char* const kBasicPitchModelHexChunks[] = {\n")

    set(_uapmd_bp_chunk_length 60000)
    set(_uapmd_bp_offset 0)
    set(_uapmd_bp_chunk_count 0)
    while(_uapmd_bp_offset LESS _uapmd_bp_hex_length)
        math(EXPR _uapmd_bp_remaining "${_uapmd_bp_hex_length} - ${_uapmd_bp_offset}")
        set(_uapmd_bp_take ${_uapmd_bp_chunk_length})
        if(_uapmd_bp_remaining LESS _uapmd_bp_chunk_length)
            set(_uapmd_bp_take ${_uapmd_bp_remaining})
        endif()
        string(SUBSTRING "${_uapmd_bp_hex}" ${_uapmd_bp_offset} ${_uapmd_bp_take} _uapmd_bp_piece)
        string(APPEND _uapmd_bp_generated "    \"${_uapmd_bp_piece}\",\n")
        math(EXPR _uapmd_bp_offset "${_uapmd_bp_offset} + ${_uapmd_bp_take}")
        math(EXPR _uapmd_bp_chunk_count "${_uapmd_bp_chunk_count} + 1")
    endwhile()

    string(APPEND _uapmd_bp_generated
            "};\n\n"
            "extern const size_t kBasicPitchModelHexChunkCount = ${_uapmd_bp_chunk_count};\n"
            "extern const size_t kBasicPitchModelByteCount = ${_uapmd_bp_byte_count};\n")

    # Written via a staging file so that reconfiguring does not touch the real
    # one, and so does not force a rebuild, unless the model actually changed.
    set(_uapmd_bp_source "${CMAKE_CURRENT_BINARY_DIR}/basic_pitch_model_data.cpp")
    file(WRITE "${_uapmd_bp_source}.staging" "${_uapmd_bp_generated}")
    configure_file("${_uapmd_bp_source}.staging" "${_uapmd_bp_source}" COPYONLY)

    add_library(uapmd-basic-pitch STATIC
            src/basic-pitch/OnnxWeights.cpp
            src/basic-pitch/EmbeddedModel.cpp
            src/basic-pitch/BasicPitchModel.cpp
            src/basic-pitch/BasicPitchTranscriber.cpp
            src/basic-pitch/BasicPitchAddin.cpp
            src/ClipTranscription.cpp
            src/NoteClipWriter.cpp
            "${_uapmd_bp_source}"
    )
    add_library(uapmd::basic-pitch ALIAS uapmd-basic-pitch)

    target_compile_features(uapmd-basic-pitch PRIVATE cxx_std_23)

    target_include_directories(uapmd-basic-pitch PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/basic-pitch
    )

    target_include_directories(uapmd-basic-pitch PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-engine/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-graph/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
            ${midicci_SOURCE_DIR}/include
    )

    target_link_libraries(uapmd-basic-pitch PRIVATE
            uapmd::uapmd-addin-core
            uapmd-data
    )

    # The convolutions are plain loops, checked against reference activations
    # rather than tuned; at the Debug default of -O0 a two-second window takes
    # long enough to look broken.
    uapmd_optimize_heavy_audio_feature_sources_in_debug(uapmd-basic-pitch
            src/basic-pitch/BasicPitchModel.cpp
            src/basic-pitch/BasicPitchTranscriber.cpp
            src/ClipTranscription.cpp
    )

endif()
