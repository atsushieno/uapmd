
add_library(uapmd-midi-service STATIC)

if(WIN32 AND UAPMD_ENABLE_WINMIDI)
    if(NOT LIBREMIDI_WINMIDI_HEADERS_ZIP)
        if(NOT UAPMD_WINMIDI_HEADERS_ARCHIVE)
            message(FATAL_ERROR "UAPMD_ENABLE_WINMIDI is ON but UAPMD_WINMIDI_HEADERS_ARCHIVE is not specified.")
        endif()
        set(_uapmd_winmidi_archive "${UAPMD_WINMIDI_HEADERS_ARCHIVE}")
        if(NOT IS_ABSOLUTE "${_uapmd_winmidi_archive}")
            cmake_path(SET _uapmd_winmidi_archive NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../${_uapmd_winmidi_archive}")
        endif()
        if(NOT EXISTS "${_uapmd_winmidi_archive}")
            message(FATAL_ERROR
                    "Expected Windows MIDI Services package at ${_uapmd_winmidi_archive}.\n"
                    "Place the NuGet archive under external/ or set UAPMD_WINMIDI_HEADERS_ARCHIVE accordingly.")
        endif()
        set(LIBREMIDI_WINMIDI_HEADERS_ZIP "${_uapmd_winmidi_archive}" CACHE FILEPATH "Path to the Windows MIDI Services header package used by libremidi" FORCE)
    endif()
endif()

target_include_directories(uapmd-midi-service PRIVATE
        ../remidy/include
        ../uapmd-plugin-hosting/include
        include
        ${miniaudio_SOURCE_DIR}
        ${choc_SOURCE_DIR}
        ${concurrentqueue_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
)

target_link_libraries(uapmd-midi-service PUBLIC
        remidy::remidy
        uapmd::uapmd-plugin-hosting
        PRIVATE
        farbot::farbot
        midicci_static
)
if(TARGET uapmd::ara-api)
    target_link_libraries(uapmd-midi-service PRIVATE uapmd::ara-api)
endif()

target_sources(uapmd-midi-service PRIVATE
        src/midi/UapmdFunctionBlock.cpp
        src/midi/UapmdMidiCISession.cpp
        src/midi/UapmdUmpMapper.cpp
        src/midi/UapmdNodeUmpMapper.cpp
)
