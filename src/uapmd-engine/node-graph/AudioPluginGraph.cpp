
#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include "AudioPluginNode.cpp"

namespace uapmd {

    class AudioPluginGraphImpl : public AudioPluginGraph {
        std::vector<std::unique_ptr<AudioPluginNodeImpl>> nodes_;
        size_t event_buffer_size_in_bytes_;

    public:
        explicit AudioPluginGraphImpl(size_t eventBufferSizeInBytes)
            : event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
        }

        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        bool removeNodeSimple(int32_t instanceId) override;
        int32_t processAudio(AudioProcessContext& process, std::function<uint8_t(int32_t)> groupResolver, std::function<void(int32_t, const uapmd_ump_t*, size_t)> eventOutputCallback) override;
        std::map<int32_t, AudioPluginNode*> plugins() override;
    };

    int32_t AudioPluginGraphImpl::processAudio(AudioProcessContext& process, std::function<uint8_t(int32_t)> groupResolver, std::function<void(int32_t, const uapmd_ump_t*, size_t)> eventOutputCallback) {
        if (nodes_.empty())
            return 0;

        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto& node = nodes_[i];
            auto instanceId = node->instanceId();

            // Drain queue to pending events
            node->drainQueueToPending();

            // Get group for this instance
            uint8_t group = 0xFF;
            if (groupResolver)
                group = groupResolver(instanceId);

            // Fill event buffer with events for this group
            auto& eventIn = process.eventIn();
            eventIn.position(0);
            node->fillEventBufferForGroup(eventIn, group);

            // Process audio
            auto status = node->instance()->processAudio(process);
            if (status != 0)
                return status;

            // Handle event output
            auto& eventOut = process.eventOut();
            if (eventOut.position() > 0) {
                if (eventOutputCallback) {
                    eventOutputCallback(
                        instanceId,
                        static_cast<uapmd_ump_t*>(eventOut.getMessages()),
                        eventOut.position()
                    );
                }
                eventOut.position(0);
            }

            if (i + 1 < nodes_.size()) {
                process.advanceToNextNode();
            }
        }
        return 0;
    }

    uapmd_status_t AudioPluginGraphImpl::appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) {
        auto node = std::make_unique<AudioPluginNodeImpl>(instanceId, instance, event_buffer_size_in_bytes_, std::move(onDelete));
        nodes_.push_back(std::move(node));
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraphImpl::removeNodeSimple(int32_t instanceId) {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i]->instanceId() == instanceId) {
                nodes_.erase(nodes_.begin() + i);
                return true;
            }
        }
        return false;
    }

    std::map<int32_t, AudioPluginNode*> AudioPluginGraphImpl::plugins() {
        std::map<int32_t, AudioPluginNode*> ret{};
        for (auto& node : nodes_) {
            ret[node->instanceId()] = node.get();
        }
        return ret;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginGraphImpl>(eventBufferSizeInBytes);
    }

}
