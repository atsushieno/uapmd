
add_library(uapmd-engine STATIC)

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
        GIT_TAG        v5.4.3
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)

CPMAddPackage(
        NAME           miniaudio
        GIT_TAG        0.11.23
        GIT_REPOSITORY https://github.com/mackron/miniaudio
        DOWNLOAD_ONLY YES
)
set(CMAKE_SKIP_INSTALL_RULES OFF)

target_include_directories(uapmd-engine PRIVATE
        ${miniaudio_SOURCE_DIR}
        ${libremidi_SOURCE_DIR}
        ${choc_SOURCE_DIR}
        ../../include
        ${midicci_SOURCE_DIR}/include
        ${concurrentqueue_SOURCE_DIR}
)

target_link_libraries(uapmd-engine
        PRIVATE
        libremidi::libremidi
        midicci_static
        PUBLIC
        remidy::remidy
        remidy::remidy-tooling
        uapmd
)

target_sources(uapmd-engine PRIVATE
        audio/ChocAudioFileFactory.cpp
        devices/AudioIODevice.cpp
        devices/DefaultDeviceIODispatcher.cpp
        devices/LibreMidiIODevice.cpp
        devices/MidiIODevice.cpp
        devices/MiniAudioIODevice.cpp
        plugin-api/RemidyAudioPluginHost.cpp
        sequencer/RealtimeSequencer.cpp
        sequencer/SequencerEngine.cpp
        sequencer/SequencerTrack.cpp
        project/UapmdProjectFileReader.cpp
        project/UapmdProjectFileWriter.cpp
)

# Add tests subdirectory
option(UAPMD_BUILD_TESTS "Build uapmd-app tests" ON)
if(UAPMD_BUILD_TESTS)
    add_subdirectory(tests)
endif()
