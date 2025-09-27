add_executable(uapmd-service)

target_include_directories(uapmd-service PRIVATE
        ../../include
        ../../src/remidy-tooling
        ../../external/choc
        ../../external/libremidi/include
        ../../external/cpplocate/source/cpplocate/include
        ${midicci_SOURCE_DIR}/include
        ${cmidi2_SOURCE_DIR}
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(uapmd-service_LIBS_PLAT
            "-framework AudioToolbox"
            "-framework AppKit"
    )
endif ()

target_link_libraries(uapmd-service PRIVATE
        uapmd
        cpplocate::cpplocate
        cpptrace::cpptrace
        libremidi::libremidi
        midicci
        ${uapmd-service_LIBS_PLAT}

)

target_sources(uapmd-service PRIVATE
        main.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
        Controller/VirtualMidiDeviceController.cpp
        Shell/CommandShell.cpp
        VirtualMidiDevices/impl/PlatformVirtualMidiDeviceImpl.cpp
        VirtualMidiDevices/PlatformVirtualMidiDevice.cpp
        VirtualMidiDevices/UapmdMidiDevice.cpp
)
