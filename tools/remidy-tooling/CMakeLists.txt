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
        ../remidy-tooling
)

target_link_libraries(remidy-tooling PRIVATE
        remidy::remidy
        cpplocate::cpplocate
        cpptrace::cpptrace
        "-framework AudioToolbox"
        "-framework AppKit"
)

target_sources(remidy-tooling PRIVATE
        PluginScanning.cpp
        PluginInstancing.cpp
)

add_library(remidy::remidy-tooling ALIAS remidy-tooling)
