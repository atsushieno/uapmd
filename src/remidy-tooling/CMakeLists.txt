add_library(remidy-tooling STATIC)

include(FetchContent)
FetchContent_Declare(
        cpptrace
        GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
        GIT_TAG        557a4a6fab3c6e7b1341d48162039e1ca0b644e7
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cpptrace)

target_include_directories(remidy-tooling PRIVATE
        ../../include
        ${choc_SOURCE_DIR}
        ../../../../external/cpplocate/source/cpplocate/include
        .
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy-tooling_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework CoreMIDI"
    )
endif ()

target_link_libraries(remidy-tooling PRIVATE
        remidy::remidy
        cpplocate::cpplocate
        cpptrace::cpptrace
        ${remidy-tooling_LIBS_PLAT}
)

target_sources(remidy-tooling PRIVATE
        src/PluginScanTool.cpp
        src/PluginInstancing.cpp
)

add_library(remidy::remidy-tooling ALIAS remidy-tooling)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()
