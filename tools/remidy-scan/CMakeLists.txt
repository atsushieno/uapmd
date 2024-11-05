add_executable(remidy-scan)

target_include_directories(remidy-scan PRIVATE
        ../../include
        ../../src/remidy-tooling
        ../../../../external/choc
        ../../../../external/cpplocate/source/cpplocate/include
)

target_link_libraries(remidy-scan PRIVATE
        remidy
        remidy-tooling
        cpplocate::cpplocate
        cpptrace::cpptrace
        "-framework AudioToolbox"
        "-framework AppKit"
)

target_sources(remidy-scan PRIVATE
        main.cpp
)
