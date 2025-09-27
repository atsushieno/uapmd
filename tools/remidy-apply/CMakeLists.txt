add_executable(remidy-apply)

target_include_directories(remidy-apply PRIVATE
        ../../include
        ${cmidi2_SOURCE_DIR}
        ../../external/choc
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
        cpplocate::cpplocate
        cpptrace::cpptrace
        ${remidy-apply_LIBS_PLAT}
)

target_sources(remidy-apply PRIVATE
        main.cpp
)
