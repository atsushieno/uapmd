add_executable(remidy-apply)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ${midicci_SOURCE_DIR}/include
        ${choc_SOURCE_DIR}
        ${cxxopts_SOURCE_DIR}/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy-apply_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(remidy-apply PRIVATE
        uapmd
        uapmd-engine
        ${remidy-apply_LIBS_PLAT}
)
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(remidy-apply PRIVATE cpptrace::cpptrace)
endif()

target_sources(remidy-apply PRIVATE
        main.cpp
)
