
set(SERVICE_APP uapmd-service)

add_executable(${SERVICE_APP}
        VirtualMidiDevices/VirtualMidiDeviceManager.cpp
        VirtualMidiDevices/VirtualMidiDeviceManager.h)

target_include_directories(${SERVICE_APP} PRIVATE
        ../../external/libremidi/include
)

target_sources(${SERVICE_APP} PRIVATE
        main.cpp
        VirtualMidiDevices/VirtualMidiDeviceManager.cpp
)
