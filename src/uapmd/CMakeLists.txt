
add_library(uapmd)

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        be1a953a4d65dafc31be5d66ccc3f20ad9b791e5
        GIT_REPOSITORY https://github.com/celtera/libremidi
)

target_include_directories(uapmd PRIVATE
        ../../external/miniaudio
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
