#include <saucer/smartview.hpp>
#include <remidy/remidy.hpp>
#include <ranges>
#include "impl/WebViewProxySaucer.hpp"
#include "impl/WebViewProxyChoc.hpp"
#include "impl/EventLoopSaucer.hpp"
#include "SaucerWebEmbedded.hpp"
#include "AudioDeviceSetup.hpp"
#include "AudioPluginSelectors.hpp"
#include "AppModel.hpp"

int main(int argc, char** argv) {
    std::filesystem::path webDir{"web"};
    std::string appTitle{"remidy-plugin-host"};
    SaucerWebEmbedded web{webDir, true};
    uapmd::WebViewProxy::Configuration config{ .enableDebugger = true };
    uapmd::WebViewProxySaucer proxy{config, web.webview()};
    EventLoopSaucer event_loop{web.app()};

    remidy_tooling::PluginScanning scanning{};
    scanning.performPluginScanning();
    uapmd::AppModel::instance().pluginScanning = &scanning;

    remidy::EventLoop::instance(event_loop);

    remidy::EventLoop::initializeOnUIThread();

    // This should be part of RemidyAudioDeviceSetupElement WebView C++ counterpart class.
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
    proxy.registerFunction("remidy_getAudioPluginEntryList", [](const std::string_view&) -> std::string {
        auto pluginList = uapmd::getPluginViewEntryList();
        auto entries = pluginList.entries();
        auto denyList = pluginList.denyList();
        auto js = choc::value::createObject("Devices",
            "entries", choc::value::createArray(entries.size(), [entries](int index) {
                auto& e = entries[index];
                return choc::value::createObject("AudioPluginEntry", "format", e.format, "id", e.id, "name", e.name, "vendor", e.vendor);
            }),
            "denyList", choc::value::createArray(denyList.size(), [denyList](int index) {
                auto& e = denyList[index];
                return choc::value::createObject("AudioPluginEntry", "format", e.format, "id", e.id, "name", e.name, "vendor", e.vendor);
            })
        );
        auto ret = choc::json::toString(js, true);
        return ret;
    });
    proxy.registerFunction("remidy_performPluginScanning", [](const std::string_view&) -> std::string {
        return std::to_string(uapmd::AppModel::instance().pluginScanning->performPluginScanning());
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

    proxy.navigateToLocalFile("web/index.html");
    proxy.evalJS("window.dispatchEvent(new Event('remidyDevicesUpdated'))");
    proxy.evalJS("window.dispatchEvent(new Event('remidyAudioPluginListUpdated'))");

    proxy.windowTitle(appTitle);
    proxy.show();

    remidy::EventLoop::start();

    return 0;
}
