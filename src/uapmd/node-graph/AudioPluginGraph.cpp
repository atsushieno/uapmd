
#include "uapmd/uapmd.hpp"
#include "AudioPluginNode.cpp"

namespace uapmd {

    class AudioPluginGraphImpl : public AudioPluginGraph {
        std::vector<std::unique_ptr<AudioPluginNodeImpl>> nodes_;
        size_t event_buffer_size_in_bytes_;
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;

    public:
        explicit AudioPluginGraphImpl(size_t eventBufferSizeInBytes)
            : event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
        }

        uapmd_status_t appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) override;
        bool removeNodeSimple(int32_t instanceId) override;
        void setGroupResolver(std::function<uint8_t(int32_t)> resolver) override;
        void setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) override;
        int32_t processAudio(AudioProcessContext& process) override;
        std::map<int32_t, AudioPluginNode*> plugins() override;
        AudioPluginNode* getPluginNode(int32_t instanceId) override;
    };

    void AudioPluginGraphImpl::setGroupResolver(std::function<uint8_t(int32_t)> resolver) {
        group_resolver_ = std::move(resolver);
    }

    void AudioPluginGraphImpl::setEventOutputCallback(std::function<void(int32_t, const uapmd_ump_t*, size_t)> callback) {
        event_output_callback_ = std::move(callback);
    }

    int32_t AudioPluginGraphImpl::processAudio(AudioProcessContext& process) {
        if (nodes_.empty())
            return 0;

        process.clearAudioOutputs();

        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto& node = nodes_[i];
            if (!node)
                continue;
            auto instanceId = node->instanceId();

            // Drain queue to pending events
            node->drainQueueToPending();

            // Get group for this instance
            uint8_t group = 0xFF;
            if (group_resolver_)
                group = group_resolver_(instanceId);

            // Fill event buffer with events for this group
            auto& eventIn = process.eventIn();
            node->fillEventBufferForGroup(eventIn, group);

            // Process audio
            auto status = node->instance()->processAudio(process);
            if (status != 0)
                return status;

            // Handle event output
            auto& eventOut = process.eventOut();
            if (eventOut.position() > 0) {
                if (event_output_callback_) {
                    event_output_callback_(
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

    AudioPluginNode* AudioPluginGraphImpl::getPluginNode(int32_t instanceId) {
        for (auto& node : nodes_)
            if (node->instanceId() == instanceId)
                return node.get();
        return nullptr;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginGraphImpl>(eventBufferSizeInBytes);
    }

}
