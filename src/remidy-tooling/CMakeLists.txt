add_library(remidy-tooling STATIC)

include(FetchContent)
FetchContent_Declare(
        cpptrace
        GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
        GIT_TAG        557a4a6fab3c6e7b1341d48162039e1ca0b644e7
)
FetchContent_MakeAvailable(cpptrace)

target_include_directories(remidy-tooling PRIVATE
        ../../include
        ../../../../external/choc
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
        PluginScanTool.cpp
        PluginInstancing.cpp
)

add_library(remidy::remidy-tooling ALIAS remidy-tooling)
