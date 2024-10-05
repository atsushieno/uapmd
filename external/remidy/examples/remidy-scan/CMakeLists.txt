add_executable(remidy-scan)


target_include_directories(remidy-scan PRIVATE
        ../../include
        ../../../../external/cpplocate/source/cpplocate/include
)

target_link_libraries(remidy-scan PRIVATE
        remidy::remidy
        cpplocate::cpplocate
)

target_sources(remidy-scan PRIVATE
        main.cpp
)
