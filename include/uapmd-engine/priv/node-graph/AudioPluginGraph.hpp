#pragma once
#include <memory>
#include <vector>

#include "uapmd/uapmd.hpp"

namespace uapmd {

    class AudioPluginGraph {
    protected:
        AudioPluginGraph() = default;

    public:
        virtual ~AudioPluginGraph() = default;
        static std::unique_ptr<AudioPluginGraph> create();

        virtual uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) = 0;
        virtual bool removeNodeSimple(int32_t instanceId) = 0;

        virtual std::map<int32_t,AudioPluginInstanceAPI*> plugins() = 0;

        virtual int32_t processAudio(AudioProcessContext& process) = 0;
    };

}
