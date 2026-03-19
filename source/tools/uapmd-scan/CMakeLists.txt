add_executable(uapmd-scan)

set(UAPMD_SCAN_PRIVATE_INCLUDES
        ../../remidy/include
        ../../remidy-tooling/include
        ${choc_SOURCE_DIR}
)

target_include_directories(uapmd-scan PRIVATE ${UAPMD_SCAN_PRIVATE_INCLUDES})

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd-scan_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(uapmd-scan PRIVATE
        remidy
        remidy-tooling
        ${uapmd-scan_LIBS_PLAT}
)
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(uapmd-scan PRIVATE cpptrace::cpptrace)
endif()

target_sources(uapmd-scan PRIVATE
        main.cpp
)
