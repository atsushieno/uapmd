
#include <iostream>
#include <ostream>

#include <remidy/remidy.hpp>

void testCreateInstance(remidy::AudioPluginFormat* format, remidy::PluginCatalogEntry* pluginId) {
    auto displayName = pluginId->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
    std::cerr << "instantiating " << displayName << std::endl;

    auto instance = format->createInstance(pluginId);
    if (!instance)
        std::cerr << "Could not instantiate plugin " << displayName << std::endl;
    else {
        delete instance;
        std::cerr << "Successfully instantiated and deleted " << displayName << std::endl;
    }
}

int main(int argc, const char * argv[]) {
    std::vector<std::string> vst3SearchPaths{};
    remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    auto formats = std::vector<remidy::AudioPluginFormat*>{&vst3};

    for (auto format : formats) {
        auto infos = format->getAvailablePlugins().getPlugins();
        for (int i = 0, n = infos.size(); i < n; ++i) {
            auto pluginId = infos[i];
            auto displayName = pluginId->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::DisplayName);
            auto vendor = pluginId->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::VendorName);
            auto url = pluginId->getMetadataProperty(remidy::PluginCatalogEntry::MetadataPropertyID::ProductUrl);
            std::cerr << "[" << i + 1 << "/" << infos.size() << "] " << displayName << " : " << vendor << " (" << url << ")" << std::endl;

            // FIXME: implement blocklist
            if (displayName.starts_with("Firefly Synth 1.8.6 VST3"))
                continue;
            if (displayName.starts_with("sfizz"))
                continue;
            if (displayName.starts_with("JEQ8")) // memory leaker
                continue;

            // FIXME: this should be unblocked

            // They can be loaded but causes: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)

            if (displayName.starts_with("Battery"))
                continue;
            if (displayName == "Kontakt")
                continue;
            if (displayName == "Kontakt 7")
                continue;

            testCreateInstance(format, pluginId);
        }

        std::cerr << "Completed." << std::endl;
    }
    return 0;
}
