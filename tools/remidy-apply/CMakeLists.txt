add_executable(remidy-apply)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ../remidy-tooling
        ../../external/choc
        ../../external/cpplocate/source/cpplocate/include
        ../../external/cxxopts/include
)

target_link_libraries(remidy-apply PRIVATE
        remidy-tooling
        cpplocate::cpplocate
        cpptrace::cpptrace
        "-framework AudioToolbox"
        "-framework AppKit"
)

target_sources(remidy-apply PRIVATE
        main.cpp
)
