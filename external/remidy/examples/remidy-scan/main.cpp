
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
            std::cout << pluginId->getDisplayName() << " : " << pluginId->getVendor() << " (" << pluginId->getUrl() << ")" << std::endl;
            auto instance = format->createInstance(pluginId);
            delete instance;
        }
        return 0;
    }
}
