add_executable(remidy-scan)

target_include_directories(remidy-scan PRIVATE
        ../../include
        ../../src/remidy-tooling
        ${choc_SOURCE_DIR}
        ${cpplocate_SOURCE_DIR}/source/cpplocate/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy-scan_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(remidy-scan PRIVATE
        remidy
        remidy-tooling
        cpptrace::cpptrace
        ${remidy-scan_LIBS_PLAT}
)

target_sources(remidy-scan PRIVATE
        main.cpp
)
