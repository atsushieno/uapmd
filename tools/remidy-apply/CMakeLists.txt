add_executable(remidy-apply)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ../../external/cmidi2
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
