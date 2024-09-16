add_executable(remidy-scan)


target_include_directories(remidy-scan PRIVATE
        ../../include
)

target_link_libraries(remidy-scan PRIVATE
        remidy::remidy
)

target_sources(remidy-scan PRIVATE
        main.cpp
)
