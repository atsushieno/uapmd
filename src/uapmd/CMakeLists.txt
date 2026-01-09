
add_library(uapmd STATIC)

set(CMAKE_SKIP_INSTALL_RULES ON)

if(WIN32 AND UAPMD_ENABLE_WINMIDI)
    if(NOT LIBREMIDI_WINMIDI_HEADERS_ZIP)
        if(UAPMD_WINMIDI_RELEASE_TAG STREQUAL "" OR UAPMD_WINMIDI_PACKAGE_NAME STREQUAL "")
            message(FATAL_ERROR "UAPMD_ENABLE_WINMIDI is ON but UAPMD_WINMIDI_RELEASE_TAG or UAPMD_WINMIDI_PACKAGE_NAME is empty.")
        endif()
        set(_uapmd_winmidi_dir "${CMAKE_BINARY_DIR}/winmidi")
        file(MAKE_DIRECTORY "${_uapmd_winmidi_dir}")
        set(_uapmd_winmidi_zip "${_uapmd_winmidi_dir}/${UAPMD_WINMIDI_PACKAGE_NAME}")
        if(NOT EXISTS "${_uapmd_winmidi_zip}")
            set(_uapmd_winmidi_url "https://github.com/microsoft/MIDI/releases/download/${UAPMD_WINMIDI_RELEASE_TAG}/${UAPMD_WINMIDI_PACKAGE_NAME}")
            message(STATUS "Downloading Windows MIDI Services headers from ${_uapmd_winmidi_url}")
            file(DOWNLOAD "${_uapmd_winmidi_url}" "${_uapmd_winmidi_zip}" SHOW_PROGRESS STATUS _uapmd_winmidi_status)
            list(GET _uapmd_winmidi_status 0 _uapmd_status_code)
            if(NOT _uapmd_status_code EQUAL 0)
                list(GET _uapmd_winmidi_status 1 _uapmd_status_message)
                message(FATAL_ERROR "Failed to download ${_uapmd_winmidi_url}: ${_uapmd_status_message}")
            endif()
        endif()
        set(LIBREMIDI_WINMIDI_HEADERS_ZIP "${_uapmd_winmidi_zip}" CACHE FILEPATH "Path to the Windows MIDI Services header package used by libremidi" FORCE)
    endif()
endif()

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
