
add_library(uapmd STATIC)

set(CMAKE_SKIP_INSTALL_RULES ON)

if(WIN32 AND UAPMD_ENABLE_WINMIDI)
    if(NOT LIBREMIDI_WINMIDI_HEADERS_ZIP)
        if(NOT UAPMD_WINMIDI_HEADERS_ARCHIVE)
            message(FATAL_ERROR "UAPMD_ENABLE_WINMIDI is ON but UAPMD_WINMIDI_HEADERS_ARCHIVE is not specified.")
        endif()
        set(_uapmd_winmidi_archive "${UAPMD_WINMIDI_HEADERS_ARCHIVE}")
        if(NOT IS_ABSOLUTE "${_uapmd_winmidi_archive}")
            cmake_path(SET _uapmd_winmidi_archive NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../../${_uapmd_winmidi_archive}")
        endif()
        if(NOT EXISTS "${_uapmd_winmidi_archive}")
            message(FATAL_ERROR
                    "Expected Windows MIDI Services package at ${_uapmd_winmidi_archive}.\n"
                    "Place the NuGet archive under external/ or set UAPMD_WINMIDI_HEADERS_ARCHIVE accordingly.")
        endif()
        set(LIBREMIDI_WINMIDI_HEADERS_ZIP "${_uapmd_winmidi_archive}" CACHE FILEPATH "Path to the Windows MIDI Services header package used by libremidi" FORCE)
    endif()
endif()

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        v5.3.1
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)
set(CMAKE_SKIP_INSTALL_RULES OFF)

CPMAddPackage(
        NAME           miniaudio
        GIT_TAG        0.11.23
        GIT_REPOSITORY https://github.com/mackron/miniaudio
        EXCLUDE_FROM_ALL
)

target_include_directories(uapmd PRIVATE
        ${miniaudio_SOURCE_DIR}
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
