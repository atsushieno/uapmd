
add_library(uapmd STATIC)

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        b18a2fc38a020e0cc4b174c257de182a4328da58
        GIT_REPOSITORY https://github.com/celtera/libremidi
)
CPMAddPackage(
        NAME           ring-buffer
        GIT_TAG        e0c7b5ee052ab67cecdf26ca20dda77363506a5d
        GIT_REPOSITORY https://github.com/jpcima/ring-buffer
)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED gtk4)
endif()

target_compile_options(uapmd PUBLIC
        ${GTK4_CFLAGS}
)
target_link_options(uapmd PUBLIC
        ${GTK4_LDFLAGS}
)

target_include_directories(uapmd PRIVATE
        ../../external/miniaudio
        ../../external/choc
        ../../include
        ${cmidi2_SOURCE_DIR}
        ${ring-buffer_SOURCE_DIR}/include
)

target_link_libraries(uapmd PRIVATE
        libremidi::libremidi
        ring_buffer
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
        AudioPluginHosting/AudioPluginHostPAL.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
        Sequencer/SequenceProcessor.cpp
        Sequencer/AudioPluginSequencer.cpp
)
