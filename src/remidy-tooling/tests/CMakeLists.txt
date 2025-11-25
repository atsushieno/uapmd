if (NOT BUILD_TESTING)
    return()
endif()

include(FetchContent)

if (WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(googletest)

add_executable(remidy-tooling-plugin-scan-tests
        PluginScanToolTests.cpp
)

target_include_directories(remidy-tooling-plugin-scan-tests PRIVATE
        ../../../include
)

target_link_libraries(remidy-tooling-plugin-scan-tests PRIVATE
        remidy-tooling
        GTest::gtest_main
)

add_test(NAME remidy-tooling-plugin-scan-tests COMMAND remidy-tooling-plugin-scan-tests)
