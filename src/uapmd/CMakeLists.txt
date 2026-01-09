
add_library(uapmd STATIC)

set(CMAKE_SKIP_INSTALL_RULES ON)
CPMAddPackage(
        NAME           libremidi
        GIT_TAG        v5.3.1
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)
set(CMAKE_SKIP_INSTALL_RULES OFF)
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
        midicci_static
)

target_sources(uapmd PRIVATE
        AudioBackend/AudioIODevice.cpp
        AudioBackend/impl/DefaultDeviceIODispatcher.cpp
        AudioBackend/MidiIODevice.cpp
        AudioBackend/impl/MiniAudioIODevice.cpp
        AudioBackend/impl/LibreMidiIODevice.cpp
        AudioBackend/impl/ChocAudioFileFactory.cpp
        AudioPluginAPI/RemidyAudioPluginHost.cpp
        AudioGraph/AudioPluginGraph.cpp
        AudioGraph/AudioPluginHostPAL.cpp
        AudioGraph/AudioPluginNode.cpp
        AudioGraph/AudioPluginTrack.cpp
        Midi/UapmdNodeUmpMapper.cpp
        Midi/UapmdMidiDevice.cpp
        Midi/UapmdMidiCISessions.cpp
        Midi/UapmdUmpMapper.cpp
        Sequencer/SequencerEngine.cpp
        Sequencer/AudioPluginSequencer.cpp
)
