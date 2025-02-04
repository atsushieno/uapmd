#pragma once

#include <filesystem>
#include <travesty/base.h>

#define kVstAudioEffectClass "Audio Module Class"

namespace remidy_vst3 {

    struct PluginClassInfo {
        std::filesystem::path bundlePath;
        std::string vendor;
        std::string url;
        std::string name;
        v3_tuid tuid{};

        PluginClassInfo(
            std::filesystem::path& bundlePath,
            std::string& vendor,
            std::string& url,
            std::string& name,
            v3_tuid tuid
        ): bundlePath(bundlePath), vendor(vendor), url(url), name(name) {
            memcpy(this->tuid, tuid, 16);
        }
    };

    bool hasModuleInfo(std::filesystem::path& bundlePath);
    std::vector<PluginClassInfo> getModuleInfo(std::filesystem::path& bundlePath);

    void* loadModuleFromVst3Path(std::filesystem::path vst3Dir);
    int32_t initializeModule(void* library);
    void unloadModule(void* library);

    void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
}
