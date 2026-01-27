
#include <string>
#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    AudioPluginNode::AudioPluginNode(
        AudioPluginInstanceAPI* nodePAL,
        int32_t instanceId,
        std::function<void()> &&onDispose
    ) : instance_id_(instanceId),
        on_dispose(onDispose) {
        node_.store(nodePAL, std::memory_order_relaxed);
        bypassed_.store(true, std::memory_order_relaxed);
    }

    AudioPluginNode::~AudioPluginNode() {
        bypassed_.store(true, std::memory_order_release);

        // Stop audio processing at the instance level
        auto* instance = node_.load(std::memory_order_acquire);
        if (instance)
            instance->bypassed(true);

        // Clear pointer before deleting instance
        node_.store(nullptr, std::memory_order_release);

        // Now safe to delete - audio thread will see null pointer
        on_dispose();
    }

    bool AudioPluginNode::bypassed() {
        return bypassed_.load(std::memory_order_acquire);
    }

    void AudioPluginNode::bypassed(bool value) {
        bypassed_.store(value, std::memory_order_release);
    }

    AudioPluginInstanceAPI* AudioPluginNode::pal() const {
        return node_.load(std::memory_order_acquire);
    }

    uapmd_status_t AudioPluginNode::processAudio(AudioProcessContext &process) {
        if (bypassed_.load(std::memory_order_acquire))
            // FIXME: maybe switch to remidy::StatusCode?
            return 0;
        // Check for null - destructor may have cleared it
        if (auto* p = node_.load(std::memory_order_acquire))
            return p->processAudio(process);
        // Already released
        return 0;
    }

    int32_t AudioPluginNode::instanceId() {
        return instance_id_;
    }

    void AudioPluginNode::loadState(std::vector<uint8_t>& state) {
        pal()->loadState(state);
    }

    std::vector<uint8_t> AudioPluginNode::saveState() {
        return pal()->saveState();
    }
}
