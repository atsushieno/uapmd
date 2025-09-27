#pragma once

#include <string>
#include <vector>
#include <functional>

namespace uapmd {
    // Simplified structures for ImGui version
    struct ParameterMetadata {
        int32_t id;
        std::string name;
        float value;
        float defaultValue;
        float minValue;
        float maxValue;
    };

    struct PresetsMetadata {
        int32_t index;
        std::string name;
    };

    // Simplified app model for ImGui version - no complex dependencies
    class SimpleAppModel {
    public:
        static void instantiate() {
            if (!instance_) {
                instance_ = new SimpleAppModel();
            }
        }

        static SimpleAppModel& instance() {
            return *instance_;
        }

        static void cleanupInstance() {
            delete instance_;
            instance_ = nullptr;
        }

        void startAudio() {
            // Placeholder for audio initialization
        }

        void instantiatePlugin(int32_t instancingId, const std::string& format, const std::string& pluginId) {
            // Placeholder for plugin instantiation
            // In real implementation, this would connect to actual plugin host
        }

        std::vector<ParameterMetadata> getParameterList(int32_t instanceId) {
            // Return placeholder parameters
            return {
                {0, "Volume", 0.75f, 0.5f, 0.0f, 1.0f},
                {1, "Pan", 0.0f, 0.0f, -1.0f, 1.0f}
            };
        }

        std::vector<PresetsMetadata> getPresetList(int32_t instanceId) {
            // Return placeholder presets
            return {
                {0, "Default"},
                {1, "Bright"},
                {2, "Warm"}
            };
        }

        void loadPreset(int32_t instanceId, int32_t presetIndex) {
            // Placeholder for preset loading
        }

    private:
        SimpleAppModel() = default;
        static SimpleAppModel* instance_;
    };
}