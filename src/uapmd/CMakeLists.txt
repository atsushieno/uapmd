
add_library(uapmd STATIC)

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        v5.3.1
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)
# No direct GTK dependency here; keep this core free of GUI toolkits

target_include_directories(uapmd PRIVATE
        ../../external/miniaudio
        ${choc_SOURCE_DIR}
        ../../include
        ${cmidi2_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
)

target_link_libraries(uapmd PUBLIC
        libremidi::libremidi
        remidy::remidy
        remidy::remidy-tooling
        midicci
)

target_sources(uapmd PRIVATE
        AudioBackend/AudioIODevice.cpp
        AudioBackend/impl/DefaultDeviceIODispatcher.cpp
        AudioBackend/MidiIODevice.cpp
        AudioBackend/impl/MiniAudioIODevice.cpp
        AudioBackend/impl/LibreMidiIODevice.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginGraph.cpp
        AudioPluginHosting/AudioPluginHostPAL.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
        Midi/UapmdMidiDevice.cpp
        Sequencer/SequenceProcessor.cpp
        Sequencer/AudioPluginSequencer.cpp
)
