
add_library(uapmd STATIC)

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        v5.2.0
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)
CPMAddPackage(
        NAME           concurrentqueue
        GIT_TAG        2f09da73d22a47dc8a89cdd4fc4c3bfae07f4284
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        EXCLUDE_FROM_ALL
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
        ${concurrentqueue_SOURCE_DIR}
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
        AudioPluginHosting/AudioPluginHostPAL.cpp
        AudioPluginHosting/impl/RemidyAudioPluginHostPAL.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
        Sequencer/SequenceProcessor.cpp
        Sequencer/AudioPluginSequencer.cpp
)
