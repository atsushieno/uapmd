add_executable(remidy-apply)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ${cmidi2_SOURCE_DIR}
        ../../external/choc
        ../../external/cpplocate/source/cpplocate/include
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
