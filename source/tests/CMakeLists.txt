enable_testing()

# Find or add GoogleTest
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
if(NOT TARGET GTest::gtest OR NOT TARGET GTest::gtest_main)
    CPMAddPackage(
        NAME googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
endif()

# uapmd-data tests
add_executable(uapmd-project-file-tests
    UapmdProjectFileTest.cpp
)

add_executable(uapmd-engine-output-tests
    SequencerEngineOutputTest.cpp
)

target_include_directories(uapmd-project-file-tests PRIVATE
        ../remidy/include
        ../remidy-tooling/include
        ../uapmd/include
        ../uapmd-graph/include
        ../uapmd-file/include
        ../uapmd-data/include
        ../uapmd-engine/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_include_directories(uapmd-engine-output-tests PRIVATE
        ../remidy/include
        ../remidy-tooling/include
        ../uapmd/include
        ../uapmd-graph/include
        ../uapmd-file/include
        ../uapmd-data/include
        ../uapmd-engine/include
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

# Discover tests
include(GoogleTest)
gtest_discover_tests(uapmd-project-file-tests)
gtest_discover_tests(uapmd-engine-output-tests)
