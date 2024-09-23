
#include <iostream>
#include <ostream>

#include <remidy/remidy.hpp>

std::vector<remidy::AudioPluginFormat*> getFormats() {
    std::vector<std::string> vst3SearchPaths{};
    static remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    return std::vector<remidy::AudioPluginFormat*>{&vst3};
}

int main(int argc, const char * argv[]) {
    for (auto format : getFormats()) {
        auto pluginIds = format->getAvailablePlugins();
        for (auto &pluginId : pluginIds) {
            // FIXME: implement blocklist
            if (pluginId->getDisplayName().starts_with("Firefly Synth 1.8.6 VST3"))
                continue;
            if (pluginId->getDisplayName().starts_with("sfizz"))
                continue;
            // FIXME: they should pass
            if (pluginId->getDisplayName().starts_with("Ozone 9"))
                continue;
            if (pluginId->getDisplayName().starts_with("Neutron 3"))
                continue;
            if (pluginId->getDisplayName().starts_with("Neoverb"))
                continue;
            if (pluginId->getDisplayName().starts_with("VocalSynth 2"))
                continue;
            if (pluginId->getDisplayName().starts_with("Tonal Balance Control 2"))
                continue;
            if (pluginId->getDisplayName().starts_with("Relay"))
                continue;
            //if (!pluginId->getVendor().starts_with("iZotope"))
            //    continue;

            std::cerr << pluginId->getDisplayName() << " : " << pluginId->getVendor() << " (" << pluginId->getUrl() << ")" << std::endl;

            auto instance = format->createInstance(pluginId);
            if (!instance)
                std::cerr << "Could not instantiate plugin " << pluginId->getDisplayName() << std::endl;
            delete instance;
        }
        return 0;
    }
}
