add_library(uapmd-app-model STATIC
        src/AppModel.cpp
        src/UapmdJSRuntime.cpp
        src/McpServer.cpp
)

# MCP: cpp-httplib for embedded HTTP server mode (desktop only)
if(NOT UAPMD_BUILDING_WASM AND NOT IOS AND NOT ANDROID)
    CPMAddPackage(
        NAME cpp-httplib
        GIT_TAG v0.18.1
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
        DOWNLOAD_ONLY YES
    )
endif()

# MCP: IXWebSocket for outbound WebSocket client mode (all platforms except Wasm —
# Wasm uses the browser's native WebSocket via a future Emscripten binding path)
if(NOT UAPMD_BUILDING_WASM)
    CPMAddPackage(
        NAME ixwebsocket
        GIT_TAG v11.4.6
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket
        OPTIONS
            "USE_TLS OFF"
            "USE_ZLIB OFF"
            "IXWEBSOCKET_INSTALL OFF"
    )
endif()

# ResEmbed: build-time resource embedding (provides the res_embed_add()
# function and ResEmbed target used below to embed the JS bridge library).
#
# When cross-compiling for WASM, ResEmbed's Generator must run on the host ABI.
# Define a helper that locates a usable host compiler even when the active
# toolchain is Emscripten (whose wrappers live inside the emsdk sysroot).
function(uapmd_detect_host_cxx OUTPUT_VAR)
    set(_uapmd_candidate "")
    if(ARGC GREATER 1)
        set(_uapmd_candidate "${ARGV1}")
    endif()
    if(_uapmd_candidate AND NOT _uapmd_candidate STREQUAL "" AND
            NOT _uapmd_candidate MATCHES "-NOTFOUND$")
        set(${OUTPUT_VAR} "${_uapmd_candidate}" PARENT_SCOPE)
        return()
    endif()

    set(_uapmd_result "")
    set(_uapmd_known_host_dirs /usr/bin /usr/local/bin /opt/homebrew/bin)
    find_program(_uapmd_probe_host_cxx
        NAMES clang++ g++
        PATHS ${_uapmd_known_host_dirs}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
        set(_uapmd_result "${_uapmd_probe_host_cxx}")
    endif()

    if(NOT _uapmd_result AND APPLE)
        execute_process(
            COMMAND xcrun --find clang++
            OUTPUT_VARIABLE _uapmd_xcrun_host_cxx
            RESULT_VARIABLE _uapmd_xcrun_status
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_uapmd_xcrun_status EQUAL 0 AND EXISTS "${_uapmd_xcrun_host_cxx}")
            set(_uapmd_result "${_uapmd_xcrun_host_cxx}")
        endif()
    endif()

    if(NOT _uapmd_result)
        find_program(_uapmd_probe_host_cxx
            NAMES clang++ g++ c++
            NO_CMAKE_FIND_ROOT_PATH)
        if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
            set(_uapmd_result "${_uapmd_probe_host_cxx}")
        endif()
    endif()

    if(NOT _uapmd_result AND WIN32)
        find_program(_uapmd_probe_host_cxx
            NAMES cl CL)
        if(_uapmd_probe_host_cxx AND NOT _uapmd_probe_host_cxx MATCHES "-NOTFOUND$")
            set(_uapmd_result "${_uapmd_probe_host_cxx}")
        endif()
    endif()

    if(_uapmd_result MATCHES "\\.emsdk/" OR _uapmd_result MATCHES "emscripten")
        set(_uapmd_result "")
    endif()

    unset(_uapmd_known_host_dirs)
    unset(_uapmd_probe_host_cxx CACHE)
    unset(_uapmd_xcrun_host_cxx)
    unset(_uapmd_xcrun_status)

    set(${OUTPUT_VAR} "${_uapmd_result}" PARENT_SCOPE)
endfunction()

if(UAPMD_BUILDING_WASM)
    uapmd_detect_host_cxx(_uapmd_host_cxx "${UAPMD_HOST_CXX_COMPILER}")
    if(_uapmd_host_cxx)
        set(UAPMD_HOST_CXX_COMPILER "${_uapmd_host_cxx}" CACHE FILEPATH
            "Host C++ compiler for build-time tools when cross-compiling" FORCE)
    endif()

    set(_uapmd_resembed_host "${RES_EMBED_HOST_CXX}")
    if(NOT _uapmd_resembed_host OR _uapmd_resembed_host STREQUAL "" OR _uapmd_resembed_host MATCHES "-NOTFOUND$")
        set(_uapmd_resembed_host "${_uapmd_host_cxx}")
        if(_uapmd_resembed_host)
            set(RES_EMBED_HOST_CXX "${_uapmd_resembed_host}" CACHE FILEPATH
                "Host C++ compiler used to build the ResEmbed Generator when cross-compiling" FORCE)
        endif()
    endif()
    if(NOT _uapmd_resembed_host OR _uapmd_resembed_host STREQUAL "" OR _uapmd_resembed_host MATCHES "-NOTFOUND$")
        message(WARNING "uapmd: UAPMD_HOST_CXX_COMPILER is unset; ResEmbed's Generator may fail. "
            "Pass -DUAPMD_HOST_CXX_COMPILER=<path> to cmake.")
    endif()
    unset(_uapmd_host_cxx)
    unset(_uapmd_resembed_host)
endif()
CPMAddPackage(
    NAME ResEmbed
    GIT_TAG 23ba82c
    GIT_REPOSITORY https://github.com/eyalamirmusic/ResEmbed
)

# Embed the JS bridge library modules (remidy-bridge.js, uapmd-api.js) so the
# module resolver works on all platforms without requiring filesystem access.
# Accessible at runtime via ResEmbed::get("<filename>", "AppJsLib").
res_embed_add(uapmd-app-model
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/remidy-bridge.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/uapmd-api.js"
    NAMESPACE AppJsLib
    CATEGORY AppJsLib
)

set(_uapmd_app_model_public_include_dirs
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${choc_SOURCE_DIR}
        ${midicci_SOURCE_DIR}/include
        ${libremidi_SOURCE_DIR}/include
        ../../remidy/include
        ../../uapmd-plugin-hosting/include
        ../../uapmd-midi-service/include
        ../../uapmd-graph/include
        ../../uapmd-file/include
        ../../uapmd-data/include
        ../../uapmd-engine/include
)

target_include_directories(uapmd-app-model
        PUBLIC
            ${_uapmd_app_model_public_include_dirs}
)

if(UAPMD_ENABLE_CPPLOCATE)
    target_include_directories(uapmd-app-model PRIVATE
            ${cpplocate_SOURCE_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/cpplocate/include
            ${cpplocate_BINARY_DIR}/source/liblocate/include
    )
endif()

set(uapmd_app_model_LIBS_PLAT)
if(WIN32 AND NOT UAPMD_BUILDING_WASM)
    list(APPEND uapmd_app_model_LIBS_PLAT ws2_32)
endif()
if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd_app_model_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
            "-framework Cocoa"
    )
elseif(IOS)
    set(uapmd_app_model_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework UIKit"
            "-framework Metal"
            "-framework MetalKit"
    )
endif()

set(_uapmd_app_model_link_libraries
        remidy
        uapmd-plugin-hosting
        uapmd-midi-service
        uapmd-file
        midicci_static
        uapmd-engine
        libremidi::libremidi
        zlib-ng
)

if(TARGET uapmd::ara)
    list(APPEND _uapmd_app_model_link_libraries uapmd::ara)
    target_compile_definitions(uapmd-app-model PUBLIC UAPMD_HAS_ARA=1)
endif()
if(UAPMD_ENABLE_CPPTRACE)
    list(APPEND _uapmd_app_model_link_libraries cpptrace::cpptrace)
endif()
if(NOT UAPMD_BUILDING_WASM)
    list(APPEND _uapmd_app_model_link_libraries ${uapmd_app_model_LIBS_PLAT})
endif()
if(UAPMD_ENABLE_CPPLOCATE)
    list(APPEND _uapmd_app_model_link_libraries ${cpplocate_LIBRARIES})
endif()

target_link_libraries(uapmd-app-model PUBLIC ${_uapmd_app_model_link_libraries})
target_compile_features(uapmd-app-model PUBLIC cxx_std_23)

# UAPMD_HAS_MCP_SERVER     — MCP feature present (all platforms)
# UAPMD_MCP_HAS_HTTP_SERVER — embedded HTTP server mode available (desktop only)
# Both are PUBLIC so uapmd-app (which still contains the GUI's MCP status UI)
# sees them transitively through its link to this target.
target_compile_definitions(uapmd-app-model PUBLIC UAPMD_HAS_MCP_SERVER=1)
if(NOT UAPMD_BUILDING_WASM)
    target_link_libraries(uapmd-app-model PRIVATE ixwebsocket)
    if(NOT IOS AND NOT ANDROID)
        target_include_directories(uapmd-app-model PRIVATE ${cpp-httplib_SOURCE_DIR})
        target_compile_definitions(uapmd-app-model PUBLIC UAPMD_MCP_HAS_HTTP_SERVER=1)
    endif()
endif()
