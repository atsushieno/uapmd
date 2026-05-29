add_library(uapmd-graph STATIC)

target_include_directories(uapmd-graph
        PUBLIC
        include
        PRIVATE
        ../uapmd/include
        ../uapmd/src
        ../remidy/include
        ${midicci_SOURCE_DIR}/include
        ${concurrentqueue_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
)

target_link_libraries(uapmd-graph
        PUBLIC
        uapmd
        PRIVATE
        farbot::farbot
)

target_sources(uapmd-graph PRIVATE
        src/node-graph/AudioPluginGraph.cpp
        src/node-graph/AudioPluginFullDAGraph.cpp
)

add_library(uapmd::uapmd-graph ALIAS uapmd-graph)
