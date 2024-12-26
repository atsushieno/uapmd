add_executable(remidy-apply)

CPMAddPackage(
        NAME           cpplocate
        GIT_TAG        b796990d8df747758dbd6217b6315b7bdbb0c27e
        GIT_REPOSITORY https://github.com/cginternals/cpplocate/
)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ${cmidi2_SOURCE_DIR}
        ../../external/choc
        ../../external/cxxopts/include
)

target_link_libraries(remidy-apply PRIVATE
        remidy-tooling
        uapmd
        cpplocate::cpplocate
        cpptrace::cpptrace
        "-framework AudioToolbox"
        "-framework AppKit"
)

target_sources(remidy-apply PRIVATE
        main.cpp
)
