#pragma once

#include "uapmd/uapmd.hpp"
#include <atomic>

namespace uapmd {
    class AudioPluginNode {
        std::atomic<AudioPluginInstanceAPI*> node_;
        std::atomic<bool> bypassed_{true}; // initial
        int32_t instance_id_;
        std::function<void()> on_dispose;

    public:
        AudioPluginNode(AudioPluginInstanceAPI* nodePAL, int32_t instanceId, std::function<void()> &&onDispose);
        virtual ~AudioPluginNode();

        AudioPluginInstanceAPI* pal() const;

        // instanceId can be used to indicate a plugin instance *across* processes i.e.
        // where pointer to the instance cannot be shared.
        int32_t instanceId();

        bool bypassed();
        void bypassed(bool value);

        uapmd_status_t processAudio(AudioProcessContext& process);

        void loadState(std::vector<uint8_t>& state);
        std::vector<uint8_t> saveState();
    };

}
