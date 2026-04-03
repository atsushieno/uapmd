project(uapmd-c-api LANGUAGES C CXX)

set(UAPMD_C_API_HEADERS
    include/c-api/uapmd-c-api.h
    include/c-api/uapmd-c-common.h
    include/c-api/uapmd-c-data.h
    include/c-api/uapmd-c-file.h
    include/c-api/uapmd-c-engine.h
    include/c-api/uapmd-c-app.h
)

set(UAPMD_C_API_SOURCES
    src/uapmd-c-api.cpp
    src/uapmd-c-data.cpp
    src/uapmd-c-file.cpp
    src/uapmd-c-engine.cpp
    src/uapmd-c-app.cpp
)

add_library(uapmd-c-api STATIC ${UAPMD_C_API_HEADERS} ${UAPMD_C_API_SOURCES})

target_include_directories(uapmd-c-api
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(uapmd-c-api
    PUBLIC
        uapmd
        uapmd-data
        uapmd-file
        uapmd-engine
)

# These modules use PRIVATE includes, so we must re-add their paths here.
target_include_directories(uapmd-c-api
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/uapmd-app
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-data/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-engine/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../uapmd-file/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../remidy-tooling/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${miniaudio_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
)
