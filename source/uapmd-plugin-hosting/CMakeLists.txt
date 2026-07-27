add_library(uapmd-plugin-hosting STATIC)

set(_UAPMD_PLUGIN_HOSTING_OLD_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
# Restore previous BUILD_SHARED_LIBS setting
if(DEFINED _UAPMD_PLUGIN_HOSTING_OLD_BUILD_SHARED_LIBS AND NOT "${_UAPMD_PLUGIN_HOSTING_OLD_BUILD_SHARED_LIBS}" STREQUAL "")
    set(BUILD_SHARED_LIBS "${_UAPMD_PLUGIN_HOSTING_OLD_BUILD_SHARED_LIBS}")
else()
    unset(BUILD_SHARED_LIBS)
endif()

target_include_directories(uapmd-plugin-hosting PRIVATE
        ../remidy/include
        include
        ${choc_SOURCE_DIR}
)
if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(uapmd-plugin-hosting PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd-plugin-hosting_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework CoreMIDI"
    )
endif ()

target_link_libraries(uapmd-plugin-hosting PRIVATE
        remidy::remidy
        ${uapmd-plugin-hosting_LIBS_PLAT}
)
if(WIN32)
    target_link_libraries(uapmd-plugin-hosting PRIVATE ws2_32)
endif()
if(UAPMD_ENABLE_CPPTRACE)
    target_link_libraries(uapmd-plugin-hosting PRIVATE cpptrace::cpptrace)
endif()
if(UAPMD_ENABLE_CPPLOCATE)
    target_link_libraries(uapmd-plugin-hosting PRIVATE ${cpplocate_LIBRARIES})
endif()

target_sources(uapmd-plugin-hosting PRIVATE
        src/RemotePluginScannerProcess.cpp
        src/PluginScanTool.cpp
        src/PluginFormatManager.cpp
        src/PluginInstancing.cpp
        src/ScanOnlyRunner.cpp
        src/ipc/InProcessScanSessionManager.cpp
        src/ipc/IpcJsonChannel.cpp
        src/ipc/TcpSocket.cpp
        src/ipc/RemoteScanSessionManager.cpp
        src/ipc/RemoteScannerServer.cpp
        src/plugin-api/RemidyAudioPluginHost.cpp
)

add_library(uapmd::uapmd-plugin-hosting ALIAS uapmd-plugin-hosting)
