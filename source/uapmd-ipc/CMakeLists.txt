add_library(uapmd-ipc STATIC)

# Remote (isolated) engine is desktop-only for now. Enable it unless we are
# targeting mobile/web platforms where POSIX shared memory or fork/exec aren't
# available.
set(UAPMD_IPC_REMOTE_ENGINE TRUE)
if(ANDROID OR IOS OR EMSCRIPTEN)
    set(UAPMD_IPC_REMOTE_ENGINE FALSE)
endif()

target_include_directories(uapmd-ipc
    PUBLIC
        include
        ${readerwriterqueue_SOURCE_DIR}
        ../remidy/include
        ../uapmd-engine/include
        ../uapmd/include
        ../uapmd-data/include
        ../uapmd-file/include
        ${midicci_SOURCE_DIR}/include
        ${choc_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
    PRIVATE
        src
)

target_link_libraries(uapmd-ipc PUBLIC uapmd-engine)

set(_UAPMD_IPC_SOURCES
    src/ThreadedEngineProxy.cpp
    src/platform/IpcSocket.cpp
)

if(UAPMD_IPC_REMOTE_ENGINE)
    list(APPEND _UAPMD_IPC_SOURCES
        src/RemoteEngineProxy.cpp
        src/EngineServer.cpp
        src/platform/SharedMemory.cpp
    )
endif()

target_sources(uapmd-ipc PRIVATE ${_UAPMD_IPC_SOURCES})

if(UAPMD_IPC_REMOTE_ENGINE)
    target_compile_definitions(uapmd-ipc PUBLIC UAPMD_HAS_REMOTE_ENGINE=1)
else()
    target_compile_definitions(uapmd-ipc PUBLIC UAPMD_HAS_REMOTE_ENGINE=0)
endif()

# Unix domain socket on POSIX needs librt only on glibc-based Linux.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(uapmd-ipc PRIVATE rt)
endif()

if(WIN32)
    target_link_libraries(uapmd-ipc PRIVATE ws2_32)
endif()
