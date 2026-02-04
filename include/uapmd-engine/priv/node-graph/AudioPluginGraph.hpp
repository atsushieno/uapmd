#pragma once
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"
#include "AudioPluginNode.hpp"

namespace uapmd {

    class AudioPluginGraph {
    protected:
        AudioPluginGraph() = default;

    public:
        virtual ~AudioPluginGraph() = default;
        static std::unique_ptr<AudioPluginGraph> create(size_t eventBufferSizeInBytes);

        virtual uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) = 0;
        virtual bool removeNodeSimple(int32_t instanceId) = 0;

        virtual std::map<int32_t, AudioPluginNode*> plugins() = 0;

        virtual int32_t processAudio(AudioProcessContext& process, std::function<uint8_t(int32_t)> groupResolver, std::function<void(int32_t, const uapmd_ump_t*, size_t)> eventOutputCallback) = 0;
    };

}
