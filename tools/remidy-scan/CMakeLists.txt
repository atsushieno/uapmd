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

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
endif()

target_compile_options(remidy-scan PRIVATE
        ${GTK3_CFLAGS}
)
target_link_options(remidy-scan PRIVATE
        ${GTK3_LDFLAGS}
)

target_link_libraries(remidy-scan PRIVATE
        remidy
        remidy-tooling
        cpplocate::cpplocate
        cpptrace::cpptrace
        ${remidy-scan_LIBS_PLAT}
)

target_sources(remidy-scan PRIVATE
        main.cpp
)
