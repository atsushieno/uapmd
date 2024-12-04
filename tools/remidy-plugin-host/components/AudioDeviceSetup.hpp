#pragma once

#include <vector>
#include <string>

#include "../WebViewProxy.hpp"

namespace uapmd {
    struct AudioInDeviceInterop {
        std::string id;
        std::string name;
    };
    struct AudioOutDeviceInterop {
        std::string id;
        std::string name;
    };
    struct MidiInDeviceInterop {
        std::string id;
        std::string name;
    };
    struct MidiOutDeviceInterop {
        std::string id;
        std::string name;
    };

    class DevicesInterop {
    public:
        std::vector<AudioInDeviceInterop> audioIn{};
        std::vector<AudioOutDeviceInterop> audioOut{};
        std::vector<MidiInDeviceInterop> midiIn{};
        std::vector<MidiOutDeviceInterop> midiOut{};
    };

    // invoked via JS callbacks too.
    DevicesInterop getDevices();
    void registerAudioDeviceSetupFeatures(WebViewProxy& proxy);
}
