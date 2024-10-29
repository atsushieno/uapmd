
set(SERVICE_APP uapmd-service)

add_subdirectory(../../external/libremidi "libremidi")

add_executable(${SERVICE_APP}
)

target_include_directories(${SERVICE_APP} PRIVATE
        ../../external/libremidi/include
        ../../include
)

target_link_libraries(${SERVICE_APP} PRIVATE
        libremidi::libremidi
        remidy::remidy
)

target_sources(${SERVICE_APP} PRIVATE
        main.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
        AudioPluginHosting/impl/AudioPluginHostImpl.cpp
        AudioPluginHosting/AudioPluginGraph.cpp
        AudioPluginHosting/AudioPluginHost.cpp
        AudioPluginHosting/AudioPluginNode.cpp
        AudioPluginHosting/AudioPluginTrack.cpp
        Configuration/AudioDeviceConfiguration.cpp
        Configuration/VirtualMidiDeviceConfiguration.cpp
        Controller/VirtualMidiDeviceController.cpp
        Shell/CommandShell.cpp
        VirtualMidiDevices/impl/PlatformVirtualMidiDeviceImpl.cpp
        VirtualMidiDevices/PlatformVirtualMidiDevice.cpp
        VirtualMidiDevices/UapmdMidiDevice.cpp
)
