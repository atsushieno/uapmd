
#include "AudioPluginFormatVST3.hpp"

namespace remidy {
    std::vector<std::string>& AudioPluginFormatVST3::getDefaultSearchPaths() {
        static std::string defaultSearchPathsVST3[] {
#if _WIN32
            std::string(getenv("HOME")) + "\\Common Files\\VST3",
            std::string(getenv("PROGRAMFILES")) + "\\.vst3"
#elif __APPLE__
            "/Library/Audio/Plug-Ins/VST3",
            std::string(getenv("HOME")) + "/Library/Audio/Plug-Ins/VST3"
#else // We assume the rest covers Linux and other Unix-y platforms
            std::string(getenv("HOME")) + "/.vst3",
            "/usr/local/lib/vst3",
            "/usr/lib/vst3"
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
    }

    bool AudioPluginFormatVST3::usePluginSearchPaths() { return true;}

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresLoadLibrary() { return YES; }

    AudioPluginFormat::ScanningStrategyValue AudioPluginFormatVST3::scanRequiresInstantiation() { return MAYBE; }

    std::vector<AudioPluginIdentifier *> AudioPluginFormatVST3::scanAllAvailablePlugins() {
        throw std::runtime_error("AudioPluginFormatVST3::scanAllAvailablePlugins() not implemented");
    }

    AudioPluginInstance * AudioPluginFormatVST3::createInstance(AudioPluginIdentifier *uniqueId) {
        throw std::runtime_error("AudioPluginFormatVST3::createInstance() not implemented");
    }
}
