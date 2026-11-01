#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>
#include <uapmd-plugin-hosting/uapmd-plugin-hosting.hpp>

namespace uapmd {
    // The render owner excludes realtime processing for this scope's lifetime.
    // Lifecycle calls are marshalled to the main thread by the engine.
    class ScopedPluginProcessingMode {
        SequencerEngine& engine_;
        bool* processing_restored_{};
        std::vector<std::pair<int32_t, uapmd_plugin_hosting::AudioProcessingMode>> previous_modes_;

    public:
        explicit ScopedPluginProcessingMode(SequencerEngine& engine, bool* processingRestored = nullptr)
            : engine_(engine), processing_restored_(processingRestored) {}
        ScopedPluginProcessingMode(const ScopedPluginProcessingMode&) = delete;
        ScopedPluginProcessingMode& operator=(const ScopedPluginProcessingMode&) = delete;
        bool hasInstances() const { return !previous_modes_.empty(); }

        ~ScopedPluginProcessingMode() {
            try {
                const auto error = restore();
                if (!error.empty())
                    remidy::Logger::global()->logError("%s", error.c_str());
            } catch (...) {
                if (processing_restored_)
                    *processing_restored_ = false;
                engine_.setEngineActive(false);
                remidy::Logger::global()->logError("Failed to restore plugin processing modes");
            }
        }

        void prepare(const std::vector<int32_t>& instanceIds) {
            previous_modes_.reserve(instanceIds.size());
            for (const auto id : instanceIds) {
                auto* instance = engine_.getPluginInstance(id);
                if (!instance)
                    continue;
                previous_modes_.emplace_back(id, instance->audioProcessingMode());
            }
            for (const auto& [id, previousMode] : previous_modes_)
                if (engine_.setAudioProcessingMode(id, uapmd_plugin_hosting::AudioProcessingMode::Offline) != 0)
                    throw std::runtime_error("Failed to select offline processing for plugin " + std::to_string(id));
        }

        std::string restore() {
            std::string error;
            auto previous = std::move(previous_modes_);
            previous_modes_.clear();
            for (const auto& [id, mode] : previous) {
                if (!engine_.getPluginInstance(id))
                    continue;
                if (engine_.setAudioProcessingMode(id, mode) != 0) {
                    if (!error.empty())
                        error += "; ";
                    error += "Failed to restore processing for plugin " + std::to_string(id);
                }
            }
            if (!error.empty()) {
                if (processing_restored_)
                    *processing_restored_ = false;
                engine_.setEngineActive(false);
            }
            return error;
        }
    };
}
