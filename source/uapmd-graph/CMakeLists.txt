add_library(uapmd-graph STATIC)

target_include_directories(uapmd-graph
        PUBLIC
        include
        PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        ../uapmd-midi-service/include
        ${midicci_SOURCE_DIR}/include
        ${concurrentqueue_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
)

target_link_libraries(uapmd-graph
        PUBLIC
        uapmd-midi-service
        PRIVATE
        farbot::farbot
)

target_sources(uapmd-graph PRIVATE
        src/AudioGraphRegistry.cpp
        src/builtin/GainNode.cpp
        src/builtin/AnalyserNode.cpp
        src/builtin/ChannelMergerNode.cpp
        src/builtin/ChannelSplitterNode.cpp
        src/node-graph/AudioPluginGraph.cpp
        src/node-graph/AudioPluginFullDAGraph.cpp
)

add_library(uapmd::uapmd-graph ALIAS uapmd-graph)
