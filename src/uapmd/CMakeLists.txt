
add_subdirectory(../../external/libremidi "libremidi")

add_library(uapmd)

target_include_directories(uapmd PRIVATE
        ../../external/libremidi/include
        ../../include
)

target_link_libraries(uapmd PRIVATE
        libremidi::libremidi
        remidy::remidy
)

target_sources(uapmd PRIVATE
        AudioBackend/AudioIOService.cpp
        AudioPluginHosting/impl/AudioPluginHostImpl.cpp
        AudioPluginHosting/AudioPluginGraph.cpp
        AudioPluginHosting/AudioPluginHost.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
)
