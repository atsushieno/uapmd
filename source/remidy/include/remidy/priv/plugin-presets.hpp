#pragma once

#include <string>
#include <vector>
#include <ranges>

namespace remidy {

    class PresetInfo {
        std::string id_, name_;
        int32_t bank_, index_;

    public:
        PresetInfo(std::string id, std::string name, int32_t bank, int32_t index)
            : id_(id), name_(name), bank_(bank), index_(index) {
        }

        std::string& id() { return id_; }
        std::string& name() { return name_; }
        int32_t bank() { return bank_; }
        int32_t index() { return index_; }
    };

    class PluginPresetsSupport {
    public:
        virtual ~PluginPresetsSupport() = default;

        virtual bool isIndexStable() = 0;
        virtual bool isIndexId() = 0;
        virtual int32_t getPresetIndexForId(std::string& id) = 0;
        virtual int32_t getPresetCount() = 0;
        virtual PresetInfo getPresetInfo(int32_t index) = 0;
        // Loads the preset at `index`, which most likely resets the state.
        virtual void loadPreset(int32_t index) = 0;
    };
}
