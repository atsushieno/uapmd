#pragma once
#include "AudioPluginFormat.hpp"
#include "AudioPluginInstance.hpp"

namespace remidy {

    class AudioPluginFormatVST3 : public DesktopAudioPluginFormat {
    public:
        class Impl;

        explicit AudioPluginFormatVST3(std::vector<std::string> &overrideSearchPaths);
        ~AudioPluginFormatVST3() override;

        bool usePluginSearchPaths() override;
        std::vector<std::string>& getDefaultSearchPaths() override;
        ScanningStrategyValue scanRequiresLoadLibrary() override;
        ScanningStrategyValue scanRequiresInstantiation() override;
        std::vector<AudioPluginIdentifier*> scanAllAvailablePlugins() override;
        AudioPluginInstance* createInstance(AudioPluginIdentifier* uniqueId) override;

    private:
        Impl *impl;
    };

}
