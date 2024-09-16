
#include <iostream>
#include <ostream>

#include <remidy/remidy.hpp>

int main(int argc, const char * argv[]) {
    std::vector<std::string> searchPaths{};
    remidy::AudioPluginFormatVST3 vst3{searchPaths};
    auto pluginIds = vst3.scanAllAvailablePlugins();
    for (auto &pluginId : pluginIds) {
        std::cout << pluginId->getDisplayName() << std::endl;
    }
    return 0;
}
