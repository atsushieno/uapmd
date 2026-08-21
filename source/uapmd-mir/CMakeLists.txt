add_library(uapmd-mir SHARED
        src/MirAddin.cpp
        src/MirLibrosaAddin.cpp
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
        )
        target_compile_features(uapmd-mir-tempo-probe PRIVATE cxx_std_23)
        target_include_directories(uapmd-mir-tempo-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${libsonare_SOURCE_DIR}/include
        )
        target_link_libraries(uapmd-mir-tempo-probe PRIVATE sonare_core)
    endif()
endif()

if(APPLE)
    deploy_uapmd_addin_library(uapmd-mir
            "$<TARGET_BUNDLE_DIR:uapmd-app-bundle>/../addins")
endif()
