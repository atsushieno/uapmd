add_executable(uapmd-apply)

target_include_directories(uapmd-apply PRIVATE
        ../../include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${cxxopts_SOURCE_DIR}/include
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd-apply_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(uapmd-apply PRIVATE
        uapmd
        uapmd-engine
        uapmd-data
        ${uapmd-apply_LIBS_PLAT}
)
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(uapmd-apply PRIVATE cpptrace::cpptrace)
endif()

target_sources(uapmd-apply PRIVATE
        main.cpp
)
