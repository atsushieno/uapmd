add_library(remidy-tooling STATIC)

set(_REMIDY_TOOLING_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
# Restore previous BUILD_SHARED_LIBS setting
if(DEFINED _REMIDY_TOOLING_OLD_BUILD_SHARED_LIBS AND NOT "${_REMIDY_TOOLING_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_REMIDY_TOOLING_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

target_include_directories(remidy-tooling PRIVATE
        ../remidy/include
        include
        ${choc_SOURCE_DIR}
)
if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(remidy-tooling PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(remidy-tooling_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework CoreMIDI"
    )
endif ()

target_link_libraries(remidy-tooling PRIVATE
        remidy::remidy
        ${remidy-tooling_LIBS_PLAT}
)
if(WIN32)
    target_link_libraries(remidy-tooling PRIVATE ws2_32)
endif()
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(remidy-tooling PRIVATE cpptrace::cpptrace)
endif()
if(UAPMD_ENABLE_CPPLOCATE)
    target_link_libraries(remidy-tooling PRIVATE ${cpplocate_LIBRARIES})
endif()

target_sources(remidy-tooling PRIVATE
        src/RemotePluginScannerProcess.cpp
        src/PluginScanTool.cpp
        src/PluginFormatManager.cpp
        src/PluginInstancing.cpp
        src/ipc/InProcessScanSessionManager.cpp
        src/ipc/IpcJsonChannel.cpp
        src/ipc/TcpSocket.cpp
        src/ipc/RemoteScanSessionManager.cpp
        src/ipc/RemoteScannerServer.cpp
)

add_library(remidy::remidy-tooling ALIAS remidy-tooling)
