
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
        GIT_REPOSITORY https://github.com/celtera/libremidi
        GIT_TAG        1c1c7ce
        EXCLUDE_FROM_ALL
)

if(EMSCRIPTEN AND TARGET libremidi)
    get_target_property(_uapmd_libremidi_interface_link_options libremidi INTERFACE_LINK_OPTIONS)
    if(_uapmd_libremidi_interface_link_options)
        set(_uapmd_libremidi_filtered_interface_link_options)
        foreach(_uapmd_libremidi_link_option IN LISTS _uapmd_libremidi_interface_link_options)
            if(NOT _uapmd_libremidi_link_option MATCHES "EXPORTED_(FUNCTIONS|RUNTIME_METHODS)")
                list(APPEND _uapmd_libremidi_filtered_interface_link_options "${_uapmd_libremidi_link_option}")
            endif()
        endforeach()
        set_target_properties(libremidi PROPERTIES
                INTERFACE_LINK_OPTIONS "${_uapmd_libremidi_filtered_interface_link_options}")
    endif()
endif()

if(WIN32)
    set(_uapmd_libremidi_error_helpers [[  static bool is_error_set(const stdx::error& err) noexcept
  {
    if (err.domain() == stdx::generic_domain)
      return stdx::error_cast<std::errc>(err) != std::errc{};

    return err != stdx::error{};
  }

  bool has_client_open_error() const noexcept { return is_error_set(client_open_); }]])
    set(_uapmd_libremidi_midi_api_hpp "${libremidi_SOURCE_DIR}/include/libremidi/detail/midi_api.hpp")
    file(READ "${_uapmd_libremidi_midi_api_hpp}" _uapmd_libremidi_midi_api_hpp_contents)
    string(REPLACE "\r\n" "\n" _uapmd_libremidi_midi_api_hpp_contents "${_uapmd_libremidi_midi_api_hpp_contents}")
    set(_uapmd_libremidi_midi_api_start [[  virtual stdx::error close_port() = 0;]])
    set(_uapmd_libremidi_midi_api_end [[  bool is_port_open() const noexcept { return port_open_; }]])
    string(FIND "${_uapmd_libremidi_midi_api_hpp_contents}" "${_uapmd_libremidi_midi_api_start}" _uapmd_libremidi_midi_api_start_pos)
    string(FIND "${_uapmd_libremidi_midi_api_hpp_contents}" "${_uapmd_libremidi_midi_api_end}" _uapmd_libremidi_midi_api_end_pos)
    if(NOT _uapmd_libremidi_midi_api_start_pos EQUAL -1 AND NOT _uapmd_libremidi_midi_api_end_pos EQUAL -1)
        string(SUBSTRING "${_uapmd_libremidi_midi_api_hpp_contents}" 0 ${_uapmd_libremidi_midi_api_start_pos} _uapmd_libremidi_midi_api_prefix)
        string(SUBSTRING "${_uapmd_libremidi_midi_api_hpp_contents}" ${_uapmd_libremidi_midi_api_end_pos} -1 _uapmd_libremidi_midi_api_suffix)
        set(_uapmd_libremidi_midi_api_hpp_contents
                "${_uapmd_libremidi_midi_api_prefix}${_uapmd_libremidi_midi_api_start}\n\n${_uapmd_libremidi_error_helpers}\n\n  stdx::error is_client_open() const noexcept { return client_open_; }\n${_uapmd_libremidi_midi_api_suffix}")
    endif()
    file(WRITE "${_uapmd_libremidi_midi_api_hpp}" "${_uapmd_libremidi_midi_api_hpp_contents}")

    set(_uapmd_libremidi_midi_in_cpp "${libremidi_SOURCE_DIR}/include/libremidi/midi_in.cpp")
    file(READ "${_uapmd_libremidi_midi_in_cpp}" _uapmd_libremidi_midi_in_cpp_contents)
    string(REPLACE "\r\n" "\n" _uapmd_libremidi_midi_in_cpp_contents "${_uapmd_libremidi_midi_in_cpp_contents}")
    string(REPLACE
            [[if (auto err = m_impl->is_client_open(); err != stdx::error{})]]
            [[if (m_impl->has_client_open_error())]]
            _uapmd_libremidi_midi_in_cpp_contents
            "${_uapmd_libremidi_midi_in_cpp_contents}"
    )
    string(REPLACE
            [[if (ret == stdx::error{})]]
            [[if (!midi_api::is_error_set(ret))]]
            _uapmd_libremidi_midi_in_cpp_contents
            "${_uapmd_libremidi_midi_in_cpp_contents}"
    )
    file(WRITE "${_uapmd_libremidi_midi_in_cpp}" "${_uapmd_libremidi_midi_in_cpp_contents}")

    set(_uapmd_libremidi_midi_out_cpp "${libremidi_SOURCE_DIR}/include/libremidi/midi_out.cpp")
    file(READ "${_uapmd_libremidi_midi_out_cpp}" _uapmd_libremidi_midi_out_cpp_contents)
    string(REPLACE "\r\n" "\n" _uapmd_libremidi_midi_out_cpp_contents "${_uapmd_libremidi_midi_out_cpp_contents}")
    string(REPLACE
            [[if (auto err = m_impl->is_client_open(); err != stdx::error{})]]
            [[if (m_impl->has_client_open_error())]]
            _uapmd_libremidi_midi_out_cpp_contents
            "${_uapmd_libremidi_midi_out_cpp_contents}"
    )
    string(REPLACE
            [[if (ret == stdx::error{})]]
            [[if (!midi_api::is_error_set(ret))]]
            _uapmd_libremidi_midi_out_cpp_contents
            "${_uapmd_libremidi_midi_out_cpp_contents}"
    )
    file(WRITE "${_uapmd_libremidi_midi_out_cpp}" "${_uapmd_libremidi_midi_out_cpp_contents}")
endif()

# Patch libremidi Emscripten backend: newer revisions of observer.hpp added a
# 'bool notify' third parameter to update(), but midi_access.hpp still calls
# with two arguments. Guard with a content-check so the patch is idempotent.
set(_uapmd_libremidi_em_observer_hpp "${libremidi_SOURCE_DIR}/include/libremidi/backends/emscripten/observer.hpp")
set(_uapmd_libremidi_em_midi_access_hpp "${libremidi_SOURCE_DIR}/include/libremidi/backends/emscripten/midi_access.hpp")
set(_uapmd_libremidi_em_midi_access_tail [[    , id.c_str()
    );
  }

  void stop_stream(int port_index)
  {
    if (!available())
      return;

    const auto& id = m_current_inputs[port_index].id;
    EM_ASM(const id = UTF8ToString($1);

           let input = globalThis.__libreMidi_access.inputs.get(id);
           input.onmidimessage = undefined;, id.c_str());
  }

  std::vector<observer_emscripten*> m_observers;
  std::vector<device_information> m_current_inputs;
  std::vector<device_information> m_current_outputs;

  std::map<int, std::vector<midi_in_emscripten*>> m_opened_inputs;

  std::map<std::string, int> m_input_indices;
  std::map<std::string, int> m_output_indices;
};

}
}
]])
file(READ "${_uapmd_libremidi_em_midi_access_hpp}" _uapmd_libremidi_em_midi_access_contents)
string(REPLACE "\r\n" "\n" _uapmd_libremidi_em_midi_access_contents "${_uapmd_libremidi_em_midi_access_contents}")
string(FIND "${_uapmd_libremidi_em_midi_access_contents}" [[  void stop_stream(int port_index)]] _uapmd_libremidi_em_has_stop_stream)
string(FIND "${_uapmd_libremidi_em_midi_access_contents}" [[    , id]] _uapmd_libremidi_em_truncated_tail_pos)
if(_uapmd_libremidi_em_has_stop_stream EQUAL -1 AND NOT _uapmd_libremidi_em_truncated_tail_pos EQUAL -1)
    string(SUBSTRING "${_uapmd_libremidi_em_midi_access_contents}" 0 ${_uapmd_libremidi_em_truncated_tail_pos} _uapmd_libremidi_em_midi_access_prefix)
    set(_uapmd_libremidi_em_midi_access_contents
            "${_uapmd_libremidi_em_midi_access_prefix}${_uapmd_libremidi_em_midi_access_tail}")
endif()
string(REPLACE
        [[return EM_ASM_INT(return typeof globalThis.__libreMidi_access !== undefined;);]]
        [[return EM_ASM_INT(return typeof globalThis.__libreMidi_access !== 'undefined';);]]
        _uapmd_libremidi_em_midi_access_contents
        "${_uapmd_libremidi_em_midi_access_contents}"
)
file(READ "${_uapmd_libremidi_em_observer_hpp}" _uapmd_libremidi_em_observer_contents)
string(FIND "${_uapmd_libremidi_em_observer_contents}" "bool notify" _uapmd_libremidi_em_has_notify)
if(NOT _uapmd_libremidi_em_has_notify EQUAL -1)
    string(REPLACE
            [[obs->update(m_current_inputs, m_current_outputs);]]
            [[obs->update(m_current_inputs, m_current_outputs, true);]]
            _uapmd_libremidi_em_midi_access_contents
            "${_uapmd_libremidi_em_midi_access_contents}"
    )
endif()
file(WRITE "${_uapmd_libremidi_em_midi_access_hpp}" "${_uapmd_libremidi_em_midi_access_contents}")

target_include_directories(uapmd-engine PRIVATE
        ${miniaudio_SOURCE_DIR}
        ${libremidi_SOURCE_DIR}
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${concurrentqueue_SOURCE_DIR}
        ${readerwriterqueue_SOURCE_DIR}
        ../remidy/include
        ../uapmd/include
        ../uapmd-graph/include
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
        uapmd-graph
        uapmd-data
)

target_sources(uapmd-engine PRIVATE
        src/devices/AudioIODevice.cpp
        src/devices/DefaultDeviceIODispatcher.cpp
        src/devices/LibreMidiIODevice.cpp
        src/devices/MidiIODevice.cpp
        src/sequencer/OfflineRenderer.cpp
        src/sequencer/ProjectSerialization.cpp
        src/sequencer/RealtimeSequencer.cpp
        src/sequencer/SequencerEngine.cpp
        src/sequencer/SequencerTrack.cpp
        src/sequencer/TimelineFacade.cpp
)

# Platform-specific audio backend
if(EMSCRIPTEN)
    target_sources(uapmd-engine PRIVATE
            src/devices/WebAudioWorkletIODevice.cpp
            src/devices/MiniaudioImpl.cpp
    )
elseif(ANDROID)
    target_sources(uapmd-engine PRIVATE
            src/devices/OboeAudioIODevice.cpp
            src/devices/MiniaudioImpl.cpp
    )
else()
    target_sources(uapmd-engine PRIVATE
            src/devices/MiniAudioIODevice.cpp
    )
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
