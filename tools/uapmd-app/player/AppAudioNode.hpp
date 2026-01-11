#pragma once

#include <cstdint>
#include <vector>

namespace uapmd_app {

    // Node type enumeration for app-level nodes
    enum class AppNodeType {
        Plugin,          // Wraps uapmd::AudioPluginNode
        AudioFileSource, // Audio file clip playback
        DeviceInput,     // Device input capture
        Generator        // Future: synth/oscillator
    };

    // Base interface for all app-level audio nodes
    class AppAudioNode {
    public:
        virtual ~AppAudioNode() = default;

        // Node identification
        virtual int32_t instanceId() const = 0;
        virtual AppNodeType nodeType() const = 0;

        // Bypass control
        virtual bool bypassed() const = 0;
        virtual void bypassed(bool value) = 0;

        // State management
        virtual std::vector<uint8_t> saveState() = 0;
        virtual void loadState(const std::vector<uint8_t>& state) = 0;
    };

} // namespace uapmd_app
