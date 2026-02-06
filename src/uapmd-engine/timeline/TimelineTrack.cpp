#include <algorithm>
#include <cstring>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    TimelineTrack::TimelineTrack(uint32_t channelCount, double sampleRate)
        : channel_count_(channelCount), sample_rate_(sampleRate) {
    }

    int32_t TimelineTrack::addClip(const ClipData& clip, std::unique_ptr<AudioFileSourceNode> sourceNode) {
        if (!sourceNode)
            return -1;

        // Add the source node to our collection
        source_nodes_.push_back(std::move(sourceNode));

        // Add the clip to the clip manager
        return clip_manager_.addClip(clip);
    }

    bool TimelineTrack::removeClip(int32_t clipId) {
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
            [sourceNodeId](const std::unique_ptr<AudioSourceNode>& node) {
                return node->instanceId() == sourceNodeId;
            });

        if (it != source_nodes_.end()) {
            source_nodes_.erase(it);
        }

        return true;
    }

    bool TimelineTrack::replaceClipSourceNode(int32_t clipId, std::unique_ptr<AudioFileSourceNode> newSourceNode) {
        if (!newSourceNode)
            return false;

        // Get the clip to find its current source node
        auto* clip = clip_manager_.getClip(clipId);
        if (!clip)
            return false;

        int32_t oldSourceNodeId = clip->sourceNodeInstanceId;
        int32_t newSourceNodeId = newSourceNode->instanceId();

        // Find and replace the source node
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [oldSourceNodeId](const std::unique_ptr<AudioSourceNode>& node) {
                return node->instanceId() == oldSourceNodeId;
            });

        if (it != source_nodes_.end()) {
            // Replace the old source node with the new one
            *it = std::move(newSourceNode);
        } else {
            // Old source node not found, just add the new one
            source_nodes_.push_back(std::move(newSourceNode));
        }

        // Update the clip's source node reference
        clip->sourceNodeInstanceId = newSourceNodeId;

        return true;
    }

    bool TimelineTrack::addDeviceInputSource(std::unique_ptr<DeviceInputSourceNode> sourceNode) {
        if (!sourceNode)
            return false;

        source_nodes_.push_back(std::move(sourceNode));
        return true;
    }

    bool TimelineTrack::removeSource(int32_t sourceId) {
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [sourceId](const std::unique_ptr<AudioSourceNode>& node) {
                return node->instanceId() == sourceId;
            });

        if (it == source_nodes_.end())
            return false;

        source_nodes_.erase(it);
        return true;
    }

    void TimelineTrack::ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount) {
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

    AudioSourceNode* TimelineTrack::findSourceNode(int32_t instanceId) {
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [instanceId](const std::unique_ptr<AudioSourceNode>& node) {
                return node->instanceId() == instanceId;
            });

        return (it != source_nodes_.end()) ? it->get() : nullptr;
    }

    void TimelineTrack::processAudio(
        const TimelineState& timeline,
        float** outputBuffers,
        uint32_t numChannels,
        int32_t frameCount,
        float** deviceInputBuffers,
        uint32_t deviceChannelCount
    ) {
        // Ensure our temporary buffers are allocated
        ensureBuffersAllocated(numChannels, frameCount);

        // Clear mixed source buffers
        for (auto& channelBuffer : mixed_source_buffers_) {
            std::memset(channelBuffer.data(), 0, frameCount * sizeof(float));
        }

        // Step 1: Build clip map for absolute position calculations
        auto allClips = clip_manager_.getAllClips();
        std::unordered_map<int32_t, const ClipData*> clipMap;
        for (auto* clip : allClips) {
            clipMap[clip->clipId] = clip;
        }

        // Step 2: Query active clips at current timeline position
        auto activeClips = clip_manager_.getActiveClipsAt(timeline.playheadPosition);

        // Step 3: Process source nodes for each active clip
        for (auto* clip : activeClips) {
            if (clip->muted)
                continue;

            // Find the source node for this clip
            auto* sourceNode = findSourceNode(clip->sourceNodeInstanceId);
            if (!sourceNode)
                continue;

            // Calculate position within the source file using anchor-aware calculation
            int64_t sourcePosition = clip->getSourcePosition(timeline.playheadPosition, clipMap);
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

        // Step 4: Process device input sources
        for (auto& sourceNode : source_nodes_) {
            if (sourceNode->nodeType() == SourceNodeType::DeviceInput) {
                auto* deviceInputNode = dynamic_cast<DeviceInputSourceNode*>(sourceNode.get());
                if (deviceInputNode && deviceInputBuffers) {
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

        // Step 5: Copy mixed source buffer to output buffers
        for (uint32_t ch = 0; ch < numChannels && ch < mixed_source_buffer_ptrs_.size(); ++ch) {
            if (outputBuffers[ch]) {
                memcpy(outputBuffers[ch], mixed_source_buffer_ptrs_[ch], frameCount * sizeof(float));
            }
        }
    }

} // namespace uapmd
