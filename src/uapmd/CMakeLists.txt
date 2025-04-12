
add_library(uapmd STATIC)

if (APPLE)
enable_language(OBJCXX)
endif ()

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
CPMAddPackage(
        NAME           cmidi2
        GIT_REPOSITORY https://github.com/atsushieno/cmidi2
        GIT_TAG        v0.7
)
CPMAddPackage(
        NAME           JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE
        GIT_TAG        8.0.7
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

if (NOT UAPMD_USE_JUCE_HOSTING)
    set(UAPMD_USE_JUCE_HOSTING 1)
endif ()

# some equivalents to juce_add_gui_app()
function (uapmd_add_juce_gui_library target)
    #target_compile_definitions(${target} PRIVATE JUCE_STANDALONE_APPLICATION=1)
    _juce_initialise_target(${target} ${ARGN})
    _juce_set_output_name(${target} $<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>)
    set_target_properties(${target} PROPERTIES JUCE_TARGET_KIND_STRING "UAPMD_LIBRARY") # name
    _juce_configure_bundle(${target} ${target})
    #_juce_configure_app_bundle(${target} ${target})
    #_juce_add_resources_rc(${target} ${target})
endfunction()

function (uapmd_add_juce_console_library target)
    _juce_initialise_target(${target} ${ARGN})

    if(NOT JUCE_ARG__NO_RESOURCERC)
        set_target_properties(${target} PROPERTIES JUCE_TARGET_KIND_STRING "ConsoleApp")
        _juce_write_configure_time_info(${target})
        _juce_add_resources_rc(${target} ${target})
        _juce_add_xcode_entitlements(${target} ${target})
    endif()
endfunction()
# end of those ^

if (UAPMD_USE_JUCE_HOSTING)
    message("WARNING: JUCE hosting backend is enabled. The resulting uapmd application must be released under the AGPLv3!!!")
    uapmd_add_juce_console_library(uapmd
            VERSION 0.1
    )
    target_include_directories(uapmd PRIVATE
            ${JUCE_SOURCE_DIR}/modules
    )
    target_compile_definitions(uapmd PRIVATE
            UAPMD_USE_JUCE_HOSTING=1
            JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    )
    if (APPLE)
        set (uapmd_JUCE_BACKEND_SOURCES
                AudioPluginHosting/impl/juce/ImportJuceModules.mm
                AudioPluginHosting/impl/juce/JuceAudioPluginHostPAL.cpp
        )
        target_link_libraries(uapmd PRIVATE
                "-framework IOKit"
                "-framework Security"
                "-framework AudioToolbox"
        )
    else ()
        set (uapmd_JUCE_BACKEND_SOURCES
                AudioPluginHosting/impl/juce/ImportJuceModules.cpp
                AudioPluginHosting/impl/juce/JuceAudioPluginHostPAL.cpp
        )
    endif ()
endif ()

target_sources(uapmd PRIVATE
        AudioBackend/AudioIODevice.cpp
        AudioBackend/DeviceIODispatcher.cpp
        AudioBackend/MidiIODevice.cpp
        AudioBackend/impl/MiniAudioIODevice.cpp
        AudioBackend/impl/LibreMidiIODevice.cpp
        AudioPluginHosting/AudioPluginGraph.cpp
        AudioPluginHosting/SequenceProcessor.cpp
        AudioPluginHosting/AudioPluginHostPAL.cpp
        AudioPluginHosting/impl/remidy/RemidyAudioPluginHostPAL.cpp
        ${uapmd_JUCE_BACKEND_SOURCES}
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
)
