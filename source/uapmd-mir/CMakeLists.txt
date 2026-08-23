# This directory holds two independent features: the opt-in music-analysis
# addins (UAPMD_ENABLE_MIR), and the Demucs stem separation addin, which is
# always built. Only the analysis half is gated, so librosa.cpp and libsonare
# are not fetched for builds that do not want the analysis commands.
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

    add_library(uapmd-mir SHARED
            src/MirAddin.cpp
            src/MirLibrosaAddin.cpp
            src/MirLibrosaAnalysis.cpp
            src/MirTempoMap.cpp
            src/MirMeterMap.cpp
    )

    add_uapmd_addin_library(uapmd-mir)

    target_compile_features(uapmd-mir PRIVATE cxx_std_23)

    target_compile_definitions(uapmd-mir PRIVATE UAPMD_ENABLE_LIBROSA=1)

    target_include_directories(uapmd-mir PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-engine/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-midi-service/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-plugin-hosting/include
            ${midicci_SOURCE_DIR}/include
    )
    target_link_libraries(uapmd-mir PRIVATE
            uapmd-engine
            uapmd-data
            librosa::librosa
    )

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

    if(APPLE)
        deploy_uapmd_addin_library(uapmd-mir
                "$<TARGET_BUNDLE_DIR:uapmd-app-bundle>/../addins")
    endif()
endif()

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
