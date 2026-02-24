add_executable(remidy-scan)

target_include_directories(remidy-scan PRIVATE
        ../../include
        ../../src/remidy-tooling
        ${choc_SOURCE_DIR}
)
if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(remidy-scan PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
    )
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy-scan_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(remidy-scan PRIVATE
        remidy
        remidy-tooling
        ${remidy-scan_LIBS_PLAT}
)
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(remidy-scan PRIVATE cpptrace::cpptrace)
endif()

target_sources(remidy-scan PRIVATE
        main.cpp
)
