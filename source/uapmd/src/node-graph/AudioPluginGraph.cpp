
#include "uapmd/uapmd.hpp"
#include "farbot/RealtimeObject.hpp"
#include "AudioPluginNodeImpl.hpp"

namespace uapmd {

    using NodeList = std::vector<std::shared_ptr<AudioPluginNodeImpl>>;
    using RTNodeList = farbot::RealtimeObject<NodeList, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

    class AudioPluginGraphImpl : public AudioPluginGraph {
        RTNodeList nodes_;
        size_t event_buffer_size_in_bytes_;
        std::function<uint8_t(int32_t)> group_resolver_;
        std::function<void(int32_t, const uapmd_ump_t*, size_t)> event_output_callback_;

    public:
        explicit AudioPluginGraphImpl(size_t eventBufferSizeInBytes)
            : event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
        }
        ~AudioPluginGraphImpl() override = default;

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
        RTNodeList::ScopedAccess<farbot::ThreadType::realtime> access(nodes_);
        auto& nodes = *access;

        if (nodes.empty()) {
            // No plugins to process, simply route timeline/device input straight to output.
            process.copyInputsToOutputs();
            return 0;
        }

        process.clearAudioOutputs();

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
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

            bool bypassed = node->instance()->bypassed();
            if (!bypassed)
                node->processInputMapping(process);
            if (bypassed) {
                // Pass audio through regardless of position so the signal is
                // preserved when a plugin is disabled.  For a synthesizer that
                // is the sole node on a track, inputs are empty and
                // copyInputsToOutputs() produces silence — which is correct.
                process.copyInputsToOutputs();
                if (i + 1 < nodes.size())
                    process.advanceToNextNode();
                continue;
            }

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

            if (i + 1 < nodes.size())
                process.advanceToNextNode();
        }
        return 0;
    }

    uapmd_status_t AudioPluginGraphImpl::appendNodeSimple(int32_t instanceId, AudioPluginInstanceAPI* instance, std::function<void()>&& onDelete) {
        auto newNode = std::make_shared<AudioPluginNodeImpl>(instanceId, instance, event_buffer_size_in_bytes_, std::move(onDelete));
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        access->push_back(std::move(newNode));
        // FIXME: define return codes
        return 0;
    }

    bool AudioPluginGraphImpl::removeNodeSimple(int32_t instanceId) {
        std::shared_ptr<AudioPluginNodeImpl> removed;
        {
            RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
            auto& nodes = *access;
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i]->instanceId() == instanceId) {
                    removed = std::move(nodes[i]);
                    nodes.erase(nodes.begin() + static_cast<ptrdiff_t>(i));
                    break;
                }
            }
        }
        // ~AudioPluginNodeImpl (and on_delete_()) runs here, outside the farbot lock.
        return removed != nullptr;
    }

    std::map<int32_t, AudioPluginNode*> AudioPluginGraphImpl::plugins() {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        std::map<int32_t, AudioPluginNode*> ret{};
        for (auto& node : *access)
            ret[node->instanceId()] = node.get();
        return ret;
    }

    AudioPluginNode* AudioPluginGraphImpl::getPluginNode(int32_t instanceId) {
        RTNodeList::ScopedAccess<farbot::ThreadType::nonRealtime> access(nodes_);
        for (auto& node : *access)
            if (node->instanceId() == instanceId)
                return node.get();
        return nullptr;
    }

    std::unique_ptr<AudioPluginGraph> AudioPluginGraph::create(size_t eventBufferSizeInBytes) {
        return std::make_unique<AudioPluginGraphImpl>(eventBufferSizeInBytes);
    }

}
