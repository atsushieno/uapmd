
add_subdirectory(../../external/libremidi "libremidi")

add_library(uapmd)

target_include_directories(uapmd PRIVATE
        ../../external/miniaudio
        ../../external/libremidi/include
        ../../external/choc
        ../../include
)

target_link_libraries(uapmd PRIVATE
        libremidi::libremidi
        remidy::remidy
        remidy::remidy-tooling
)

target_sources(uapmd PRIVATE
        AudioBackend/AudioIODevice.cpp
        AudioBackend/DeviceIODispatcher.cpp
        AudioBackend/MidiIODevice.cpp
        AudioBackend/impl/MiniAudioIODevice.cpp
        AudioBackend/impl/LibreMidiIODevice.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginGraph.cpp
        AudioPluginHosting/AudioPluginSequencer.cpp
        AudioPluginHosting/AudioPluginHostPAL.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
)
