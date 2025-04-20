
#include "AudioPluginSelectors.hpp"
#include "../AppModel.hpp"
#include "choc/text/choc_JSON.h"

// invoked by JS, via WebView registered function `remidy_getAudioPluginEntryList()`
uapmd::AudioPluginViewEntryList uapmd::getPluginViewEntryList() {
    auto& catalog = uapmd::AppModel::instance().sequencer().catalog();
    AudioPluginViewEntryList ret{catalog};
    return ret;
}

// Part of WebView C++ backend class for RemidyAudioPluginSelectors.js.
void uapmd::registerPluginViewEntryListFeatures(remidy::webui::WebViewProxy& proxy) {
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

    static std::filesystem::path emptyPath{""};
    proxy.registerFunction("remidy_performPluginScanning", [](const std::string_view& json) -> std::string {
        bool rescan = json == "true";
        uapmd::AppModel::instance().sequencer().performPluginScanning(rescan);
        return "";
    });
}