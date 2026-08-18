enable_testing()

# Find or add GoogleTest
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest." FORCE)
if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
    CPMAddPackage(
        NAME googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        EXCLUDE_FROM_ALL
    )
endif()

# uapmd-data tests
add_executable(uapmd-project-file-tests
    UapmdProjectFileTest.cpp
)

add_executable(uapmd-engine-output-tests
    SequencerEngineOutputTest.cpp
)

add_executable(uapmd-app-layer-undo-integration-tests
    ../tools/uapmd-app-model/tests/UndoIntegrationTest.cpp
)

target_include_directories(uapmd-project-file-tests PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        ../uapmd-midi-service/include
        ../uapmd-graph/include
        ../uapmd-file/include
        ../uapmd-data/include
        ../uapmd-engine/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_include_directories(uapmd-engine-output-tests PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        ../uapmd-midi-service/include
        ../uapmd-graph/include
        ../uapmd-file/include
        ../uapmd-data/include
        ../uapmd-engine/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_include_directories(uapmd-app-layer-undo-integration-tests PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        ../uapmd-midi-service/include
        ../uapmd-graph/include
        ../uapmd-file/include
        ../uapmd-data/include
        ../uapmd-engine/include
        ../tools/uapmd-app-model/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-project-file-tests
    uapmd-data
    GTest::gtest_main
    GTest::gtest
)

target_link_libraries(uapmd-engine-output-tests
    uapmd-engine
    GTest::gtest_main
    GTest::gtest
)

target_link_libraries(uapmd-app-layer-undo-integration-tests
    uapmd-app-model
    GTest::gtest_main
    GTest::gtest
)

# Discover tests
include(GoogleTest)
gtest_discover_tests(uapmd-project-file-tests)
gtest_discover_tests(uapmd-engine-output-tests)
gtest_discover_tests(uapmd-app-layer-undo-integration-tests)
