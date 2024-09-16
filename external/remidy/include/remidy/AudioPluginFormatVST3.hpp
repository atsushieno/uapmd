#pragma once
#include "AudioPluginFormat.hpp"
#include "AudioPluginInstance.hpp"

namespace remidy {

    class AudioPluginFormatVST3 : public DesktopAudioPluginFormat {
    public:
        explicit AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths);

        bool usePluginSearchPaths() override;
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<AudioPluginIdentifier*> scanAllAvailablePlugins() override;
        AudioPluginInstance* createInstance(AudioPluginIdentifier* uniqueId) override;
    };

    class AudioPluginInstanceVST3 : public AudioPluginInstance {
    public:
        AudioPluginInstanceVST3() = default;
    };

}
