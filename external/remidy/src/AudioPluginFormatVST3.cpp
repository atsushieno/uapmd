
#include <cassert>
#include <dlfcn.h>
#include <iostream>
#include <travesty/factory.h>
#include "VST3Helper.hpp"
#include "AudioPluginFormatVST3.hpp"

namespace remidy {

    class AudioPluginIdentifierVST3 : public AudioPluginIdentifier {
    private:
        PluginClassInfo info;
        std::string idString{};
    public:
        std::string & getVendor() override;

        std::string & getUrl() override;

        std::string& getUniqueId() override;

        std::string& getDisplayName() override;

    private:
    public:
        explicit AudioPluginIdentifierVST3(PluginClassInfo& info) : info(info) {
            idString = reinterpret_cast<char *>(info.tuid);
        }
    };

    std::string & AudioPluginIdentifierVST3::getVendor() { return info.vendor; }
    std::string & AudioPluginIdentifierVST3::getUrl() { return info.url; }
    std::string& AudioPluginIdentifierVST3::getUniqueId() { return idString; }
    std::string & AudioPluginIdentifierVST3::getDisplayName() { return info.className; }

    class AudioPluginFormatVST3::Impl {
        AudioPluginFormatVST3* owner;

    public:
        explicit Impl(AudioPluginFormatVST3* owner) : owner(owner) {}

        std::vector<std::unique_ptr<AudioPluginIdentifierVST3>> plugin_list_cache{};
        void scanAllAvailablePlugins();
    };

    std::vector<std::string>& AudioPluginFormatVST3::getDefaultSearchPaths() {
        static std::string defaultSearchPathsVST3[] = {
#if _WIN32
            std::string(getenv("LOCALAPPDATA")) + "\\Programs\\Common\\VST3",
            std::string(getenv("PROGRAMFILES")) + "\\Common Files\\VST3",
            std::string(getenv("PROGRAMFILES(x86)")) + "\\Common Files\\VST3"
#elif __APPLE__
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/VST3",
            "/Library/Audio/Plug-Ins/VST3",
            "/Network/Library/Audio/Plug-Ins/VST3"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.vst3",
            "/usr/lib/vst3",
            "/usr/local/lib/vst3"
#endif
        };
        static std::vector<std::string> ret = [] {
            std::vector<std::string> paths{};
            paths.append_range(defaultSearchPathsVST3);
            return paths;
        }();
        return ret;
    }

    AudioPluginFormatVST3::AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths)
        : DesktopAudioPluginFormat() {
        impl = new Impl(this);
    }
    AudioPluginFormatVST3::~AudioPluginFormatVST3() {
        delete impl;
    }


    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return YES; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return MAYBE; }

    std::vector<AudioPluginIdentifier*> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        std::vector<AudioPluginIdentifier*> ret{};
        impl->scanAllAvailablePlugins();
        for (auto& id : impl->plugin_list_cache)
            ret.emplace_back(id.get());
        return ret;
    }

    void AudioPluginFormatVST3::Impl::scanAllAvailablePlugins() {
        std::vector<PluginClassInfo> infos;
        for (auto &path : owner->getDefaultSearchPaths()) {
            std::filesystem::path dir{path};
            if (is_directory(dir)) {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    if (!strcasecmp(entry.path().extension().c_str(), ".vst3")) {
                        scanAllAvailablePluginsFromLibrary(entry.path(), infos);
                    }
                }
            }
        }
        for (auto &id : plugin_list_cache)
            id.reset();
        plugin_list_cache.clear();
        for (auto &info : infos)
            plugin_list_cache.emplace_back(std::make_unique<AudioPluginIdentifierVST3>(info));
    }

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
        v3_plugin_base* plugin;
    public:
        explicit AudioPluginInstanceVST3(v3_plugin_base* plugin) : plugin(plugin) {
        }
    };

    AudioPluginInstance * AudioPluginFormatVST3::createInstance(AudioPluginIdentifier *uniqueId) {
        throw std::runtime_error("AudioPluginFormatVST3::createInstance() not implemented");
    }
}
