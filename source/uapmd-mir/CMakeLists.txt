# Only reached when UAPMD_ENABLE_MIR is on: source/CMakeLists.txt gates the
# add_subdirectory() so librosa.cpp and libsonare are not fetched or built for
# builds that do not want the analysis commands.
if(NOT UAPMD_ENABLE_MIR)
    message(FATAL_ERROR "uapmd-mir requires UAPMD_ENABLE_MIR")
endif()

set(_UAPMD_MIR_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

if(UAPMD_ENABLE_LIBSONARE AND NOT WIN32 AND NOT IOS AND NOT EMSCRIPTEN)
    CPMAddPackage(
            NAME           libsonare
            GIT_TAG        v1.7.2
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
