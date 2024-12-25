#include <ranges>
#include "AudioDeviceSetup.hpp"
#include "choc/text/choc_JSON.h"

uapmd::DevicesInterop uapmd::getDevices() {
    DevicesInterop ret {
            .audioIn = { AudioInDeviceInterop{ .id = "0_0", .name = "default audio in from C++" } },
            .audioOut = { AudioOutDeviceInterop{ .id = "0_1", .name = "default audio out from C++" } },
            .midiIn = { MidiInDeviceInterop{ .id = "0_0", .name = "stub midi in from C++" } },
            .midiOut = { MidiOutDeviceInterop{ .id = "0_1", .name = "stub midi out from C++" } }
    };
    return ret;
}

void uapmd::registerAudioDeviceSetupFeatures(remidy::webui::WebViewProxy& proxy) {
#if 1 // commonized
    proxy.registerFunction("remidy_getDevices", [](const std::string_view&) -> std::string {
        auto devices = uapmd::getDevices();
        auto audioIn = devices.audioIn | std::views::filter([](auto& e) { return !e.id.empty(); })
                       | std::ranges::to<std::vector>();
        auto audioOut = devices.audioOut | std::views::filter([](auto& e) { return !e.id.empty(); })
                        | std::ranges::to<std::vector>();
        auto midiIn = devices.midiIn | std::views::filter([](auto& e) { return !e.id.empty(); })
                      | std::ranges::to<std::vector>();
        auto midiOut = devices.midiOut | std::views::filter([](auto& e) { return !e.id.empty(); })
                       | std::ranges::to<std::vector>();
        auto js = choc::value::createObject("Devices",
                                            "audioIn", choc::value::createArray(audioIn.size(), [audioIn](int index) { return choc::value::createObject("AudioDeviceIn", "id", audioIn[index].id, "name", audioIn[index].name); }),
                                            "audioOut", choc::value::createArray(audioOut.size(), [audioOut](int index) { return choc::value::createObject("AudioDeviceOut", "id", audioOut[index].id, "name", audioOut[index].name); }),
                                            "midiIn", choc::value::createArray(midiIn.size(), [midiIn](int index) { return choc::value::createObject("MidiDeviceIn", "id", midiIn[index].id, "name", midiIn[index].name); }),
                                            "midiOut", choc::value::createArray(midiOut.size(), [midiOut](int index) { return choc::value::createObject("MidiDeviceOut", "id", midiOut[index].id, "name", midiOut[index].name); })
        );
        auto ret = choc::json::toString(js, true);
        return ret;
    });
#else
    auto& webview = web.webview();

    proxy.registerFunction("remidy_getDevices", uapmd::WebViewProxy::ValueType::Map, [&](const std::vector<uapmd::WebViewProxy::Value*>&) -> uapmd::WebViewProxy::Value* {
        auto devices = getDevices();
        //return devices;

        std::map<std::string, uapmd::WebViewProxy::Value*> devicesJS{};
        std::vector<std::map<std::string, uapmd::WebViewProxy::Value*>> audioInJS{};
        for (auto& d : devices.audioIn)
            if (!d.id.empty())
                audioInJS.emplace_back(proxy.toValue(std::map<std::string, uapmd::WebViewProxy::Value*> {
                    "id", proxy.toValue(d.id),
                    "name", proxy.toValue(d.name)
                }));
        devicesJS["audioIn"] = proxy.toValue(audioInJS);
        devicesJS["audioOut"] = proxy.toValue(audioOutJS);
        devicesJS["midiIn"] = proxy.toValue(midiInJS);
        devicesJS["midiOut"] = proxy.toValue(midiOutJS);
        return proxy.toValue(ret);
    });
#endif

}