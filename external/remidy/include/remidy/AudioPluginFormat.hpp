#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AudioPluginLibrary.hpp"

namespace remidy {

    class AudioPluginInstance;

    class AudioPluginFormat {

    protected:
        virtual ~AudioPluginFormat() = default;

    public:
        enum ScanningStrategyValue {
            NO,
            MAYBE,
            YES
        };

        virtual bool usePluginSearchPaths() = 0;
        virtual std::vector<std::string>& getDefaultSearchPaths() = 0;
        virtual ScanningStrategyValue scanRequiresLoadLibrary() = 0;
        virtual ScanningStrategyValue scanRequiresInstantiation() = 0;
        virtual std::vector<AudioPluginIdentifier*> scanAllAvailablePlugins() = 0;
        virtual AudioPluginInstance* createInstance(AudioPluginIdentifier* uniqueId) = 0;
    };

    class DesktopAudioPluginFormat : public AudioPluginFormat {
    protected:
        explicit DesktopAudioPluginFormat() = default;

        std::vector<std::string> overrideSearchPaths{};

        std::vector<std::string>& getOverrideSearchPaths() { return overrideSearchPaths; }
        void addSearchPath(const std::string& path) { overrideSearchPaths.emplace_back(path); }
    };
} // namespace