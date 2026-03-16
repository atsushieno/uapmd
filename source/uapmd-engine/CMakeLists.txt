
add_library(uapmd-engine STATIC)

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

CPMAddPackage(
        NAME           libremidi
        GIT_TAG        v5.4.3
        GIT_REPOSITORY https://github.com/celtera/libremidi
        EXCLUDE_FROM_ALL
)

if(EMSCRIPTEN AND TARGET libremidi)
    get_target_property(_libremidi_link_opts libremidi INTERFACE_LINK_OPTIONS)
    if(_libremidi_link_opts AND _libremidi_link_opts MATCHES "EXPORTED_FUNCTIONS")
        list(FILTER _libremidi_link_opts EXCLUDE REGEX "EXPORTED_FUNCTIONS")
        if(_libremidi_link_opts)
            set_property(TARGET libremidi PROPERTY INTERFACE_LINK_OPTIONS "${_libremidi_link_opts}")
        else()
            set_property(TARGET libremidi PROPERTY INTERFACE_LINK_OPTIONS "")
        endif()
    endif()
endif()

target_include_directories(uapmd-engine PRIVATE
        ${miniaudio_SOURCE_DIR}
        ${libremidi_SOURCE_DIR}
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${concurrentqueue_SOURCE_DIR}
        ../remidy/include
        ../uapmd/include
        ../uapmd-file/include
        ../uapmd-data/include
        include
)

target_link_libraries(uapmd-engine
        PRIVATE
        libremidi::libremidi
        midicci_static
        PUBLIC
        remidy::remidy
        remidy::remidy-tooling
        uapmd
        uapmd-data
)

target_sources(uapmd-engine PRIVATE
        src/devices/AudioIODevice.cpp
        src/devices/DefaultDeviceIODispatcher.cpp
        src/devices/LibreMidiIODevice.cpp
        src/devices/MidiIODevice.cpp
        src/sequencer/OfflineRenderer.cpp
        src/sequencer/RealtimeSequencer.cpp
        src/sequencer/SequencerEngine.cpp
        src/sequencer/SequencerTrack.cpp
        src/sequencer/TimelineFacade.cpp
)

# Platform-specific audio backends.
# Phase 3: WASM uses the native Emscripten AudioWorklet API.
if(EMSCRIPTEN)
    target_sources(uapmd-engine PRIVATE src/devices/WebAudioWorkletIODevice.cpp)
elseif(ANDROID)
    target_sources(uapmd-engine PRIVATE src/devices/OboeAudioIODevice.cpp)
else()
    target_sources(uapmd-engine PRIVATE src/devices/MiniAudioIODevice.cpp)
endif()

if(ANDROID)
    find_package(oboe REQUIRED CONFIG)
    target_link_libraries(uapmd-engine PRIVATE oboe::oboe)
endif()

if(IOS)
    # miniaudio.h on Apple pulls in AVFoundation (an ObjC framework).
    # Files that include MiniAudioIODevice.hpp (which includes miniaudio.h) must
    # be compiled as Objective-C++ so the ObjC runtime is available.
    set_source_files_properties(
        src/devices/AudioIODevice.cpp
        src/devices/MiniAudioIODevice.cpp
        PROPERTIES LANGUAGE OBJCXX
    )
endif()
