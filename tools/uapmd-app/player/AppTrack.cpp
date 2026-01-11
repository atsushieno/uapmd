#include "AppTrack.hpp"
#include <algorithm>
#include <cstring>

namespace uapmd_app {

    AppTrack::AppTrack(uapmd::AudioPluginTrack* uapmdTrack, int32_t sampleRate)
        : uapmd_track_(uapmdTrack), sample_rate_(sampleRate) {
    }

    int32_t AppTrack::addClip(const ClipData& clip, std::unique_ptr<AppAudioFileSourceNode> sourceNode) {
        if (!sourceNode)
            return -1;

        // Add the source node to our collection
        source_nodes_.push_back(std::move(sourceNode));

        // Add the clip to the clip manager
        return clip_manager_.addClip(clip);
    }

    bool AppTrack::removeClip(int32_t clipId) {
        // Get the clip to find its source node
        auto* clip = clip_manager_.getClip(clipId);
        if (!clip)
            return false;

        int32_t sourceNodeId = clip->sourceNodeInstanceId;

        // Remove the clip
        if (!clip_manager_.removeClip(clipId))
            return false;

        // Remove the associated source node
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [sourceNodeId](const std::unique_ptr<AppSourceNode>& node) {
                return node->instanceId() == sourceNodeId;
            });

        if (it != source_nodes_.end()) {
            source_nodes_.erase(it);
        }

        return true;
    }

    bool AppTrack::addDeviceInputSource(std::unique_ptr<AppDeviceInputSourceNode> sourceNode) {
        if (!sourceNode)
            return false;

        source_nodes_.push_back(std::move(sourceNode));
        return true;
    }

    bool AppTrack::removeSource(int32_t sourceId) {
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [sourceId](const std::unique_ptr<AppSourceNode>& node) {
                return node->instanceId() == sourceId;
            });

        if (it == source_nodes_.end())
            return false;

        source_nodes_.erase(it);
        return true;
    }

    void AppTrack::ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount) {
        // Resize buffers if needed
        if (mixed_source_buffers_.size() != numChannels ||
            (numChannels > 0 && mixed_source_buffers_[0].size() < static_cast<size_t>(frameCount))) {

            mixed_source_buffers_.clear();
            mixed_source_buffers_.resize(numChannels);

            for (auto& channelBuffer : mixed_source_buffers_) {
                channelBuffer.resize(frameCount);
            }

            // Update buffer pointers
            mixed_source_buffer_ptrs_.clear();
            mixed_source_buffer_ptrs_.reserve(numChannels);
            for (auto& channelBuffer : mixed_source_buffers_) {
                mixed_source_buffer_ptrs_.push_back(channelBuffer.data());
            }
        }
    }

    AppSourceNode* AppTrack::findSourceNode(int32_t instanceId) {
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [instanceId](const std::unique_ptr<AppSourceNode>& node) {
                return node->instanceId() == instanceId;
            });

        return (it != source_nodes_.end()) ? it->get() : nullptr;
    }

    void AppTrack::processAudioWithTimeline(
        const TimelineState& timeline,
        float** deviceInputBuffers,
        uint32_t deviceChannelCount,
        int32_t frameCount
    ) {
        if (!uapmd_track_)
            return;

        // Determine number of channels (assume stereo for now, could be configurable)
        const uint32_t numChannels = 2;

        // Ensure our temporary buffers are allocated
        ensureBuffersAllocated(numChannels, frameCount);

        // Clear mixed source buffers
        for (auto& channelBuffer : mixed_source_buffers_) {
            std::memset(channelBuffer.data(), 0, frameCount * sizeof(float));
        }

        // Step 1: Query active clips at current timeline position
        auto activeClips = clip_manager_.getActiveClipsAt(timeline.playheadPosition);

        // Step 2: Process source nodes for each active clip
        for (auto* clip : activeClips) {
            if (clip->muted)
                continue;

            // Find the source node for this clip
            auto* sourceNode = findSourceNode(clip->sourceNodeInstanceId);
            if (!sourceNode)
                continue;

            // Calculate position within the source file
            int64_t sourcePosition = clip->getSourcePosition(timeline.playheadPosition);
            if (sourcePosition < 0)
                continue;

            // Seek source node to correct position
            sourceNode->seek(sourcePosition);

            // Set playing state
            sourceNode->setPlaying(timeline.isPlaying);

            // Create temporary buffers for this source
            std::vector<std::vector<float>> tempBuffers(numChannels);
            std::vector<float*> tempBufferPtrs;
            tempBufferPtrs.reserve(numChannels);

            for (auto& buf : tempBuffers) {
                buf.resize(frameCount);
                tempBufferPtrs.push_back(buf.data());
            }

            // Process the source node
            sourceNode->processAudio(tempBufferPtrs.data(), numChannels, frameCount);

            // Mix into our mixed source buffer with gain applied
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                for (int32_t frame = 0; frame < frameCount; ++frame) {
                    mixed_source_buffers_[ch][frame] += tempBuffers[ch][frame] * static_cast<float>(clip->gain);
                }
            }
        }

        // Step 3: Process device input sources
        for (auto& sourceNode : source_nodes_) {
            if (sourceNode->nodeType() == AppNodeType::DeviceInput) {
                auto* deviceInputNode = dynamic_cast<AppDeviceInputSourceNode*>(sourceNode.get());
                if (deviceInputNode) {
                    // Set device input buffers for this node
                    deviceInputNode->setDeviceInputBuffers(deviceInputBuffers, deviceChannelCount);
                    deviceInputNode->setPlaying(timeline.isPlaying);

                    // Create temporary buffers for device input
                    std::vector<std::vector<float>> tempBuffers(numChannels);
                    std::vector<float*> tempBufferPtrs;
                    tempBufferPtrs.reserve(numChannels);

                    for (auto& buf : tempBuffers) {
                        buf.resize(frameCount);
                        tempBufferPtrs.push_back(buf.data());
                    }

                    // Process device input
                    deviceInputNode->processAudio(tempBufferPtrs.data(), numChannels, frameCount);

                    // Mix into our mixed source buffer
                    for (uint32_t ch = 0; ch < numChannels; ++ch) {
                        for (int32_t frame = 0; frame < frameCount; ++frame) {
                            mixed_source_buffers_[ch][frame] += tempBuffers[ch][frame];
                        }
                    }
                }
            }
        }

        // Step 4: TODO - Copy mixed source buffer into uapmd track's input buffer
        // This will be done when we integrate with AppModel and have access to the AudioProcessContext

        // Step 5: Call uapmd track's processAudio()
        // This will be done by AppModel/SequencerEngine with the proper context
        // For now, we've prepared the mixed source buffer that the caller can use
    }

} // namespace uapmd_app
