
#include <iostream>
#include <ostream>

#include <remidy/remidy.hpp>

int main(int argc, const char * argv[]) {
    std::vector<std::string> vst3SearchPaths{};
    remidy::AudioPluginFormatVST3 vst3{vst3SearchPaths};
    auto formats = std::vector<remidy::AudioPluginFormat*>{&vst3};

    for (auto format : formats) {
        auto pluginIds = format->getAvailablePlugins();
        for (int i = 0, n = pluginIds.size(); i < n; ++i) {
            auto pluginId = pluginIds[i];
            std::cerr << "[" << i + 1 << "/" << pluginIds.size() << "] " << pluginId->getDisplayName() << " : " << pluginId->getVendor() << " (" << pluginId->getUrl() << ")" << std::endl;

            // FIXME: implement blocklist
            if (pluginId->getDisplayName().starts_with("Firefly Synth 1.8.6 VST3"))
                continue;
            if (pluginId->getDisplayName().starts_with("sfizz"))
                continue;
            if (pluginId->getDisplayName().starts_with("JEQ8")) // memory leaker
                continue;

            // FIXME: this should be unblocked

            // Their terminate() causes SIGKILL
            if (pluginId->getVendor() == "Native Instruments")
                continue;
            // They also cause: Process finished with exit code 134 (interrupted by signal 6:SIGABRT)
            if (pluginId->getDisplayName().starts_with("Battery"))
                continue;
            if (pluginId->getDisplayName() == "Kontakt")
                continue;
            if (pluginId->getDisplayName() == "Kontakt 7")
                continue;

            // Process finished with exit code 137 (interrupted by signal 9:SIGKILL)
            // it's probably due to mishandling of instantiation (It fails due to setIOMode() timing).
            if (pluginId->getDisplayName().starts_with("AIDA-X"))
                continue;

            // They somehow need AudioPluginLibraryPool::RetentionPolicy::Retain
            /*
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
            */

            std::cerr << "instantiating " << pluginId->getDisplayName() << std::endl;

            auto instance = format->createInstance(pluginId);
            if (!instance)
                std::cerr << "Could not instantiate plugin " << pluginId->getDisplayName() << std::endl;
            delete instance;

            if (!pluginId->getDisplayName().empty())
                std::cerr << pluginId->getDisplayName() << " done." << std::endl;
        }

        std::cerr << "Completed." << std::endl;
    }
    return 0;
}
