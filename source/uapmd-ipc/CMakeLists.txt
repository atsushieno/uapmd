add_library(uapmd-ipc STATIC)

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

target_sources(uapmd-ipc PRIVATE
    src/ThreadedEngineProxy.cpp
    src/RemoteEngineProxy.cpp
    src/EngineServer.cpp
    src/platform/SharedMemory.cpp
    src/platform/IpcSocket.cpp
)

# Unix domain socket on POSIX needs rt on Linux for shm_open
if(UNIX AND NOT APPLE)
    target_link_libraries(uapmd-ipc PRIVATE rt)
endif()

if(WIN32)
    target_link_libraries(uapmd-ipc PRIVATE ws2_32)
endif()
