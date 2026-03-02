#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    TimelineTrack::TimelineTrack(uint32_t channelCount, double sampleRate, uint32_t bufferSizeInFrames)
        : channel_count_(channelCount), sample_rate_(sampleRate) {
        source_nodes_snapshot_ = std::make_shared<SourceNodeList>();

        // Pre-allocate buffers to avoid real-time allocations
        reconfigureBuffers(channelCount, bufferSizeInFrames);
    }

    int32_t TimelineTrack::addClip(const ClipData& clip, std::unique_ptr<AudioFileSourceNode> sourceNode) {
        if (!sourceNode)
            return -1;

        auto sharedNode = std::shared_ptr<AudioFileSourceNode>(std::move(sourceNode));

        {
            std::lock_guard<std::mutex> lock(source_nodes_mutex_);
            source_nodes_.push_back(sharedNode);
            rebuildSourceNodeSnapshotLocked();
        }

        // Add the clip to the clip manager
        return clip_manager_.addClip(clip);
    }

    int32_t TimelineTrack::addClip(const ClipData& clip, std::unique_ptr<MidiSourceNode> sourceNode) {
        if (!sourceNode)
            return -1;

        auto sharedNode = std::shared_ptr<MidiSourceNode>(std::move(sourceNode));

        {
            std::lock_guard<std::mutex> lock(source_nodes_mutex_);
            source_nodes_.push_back(sharedNode);
            rebuildSourceNodeSnapshotLocked();
        }

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

        {
            std::lock_guard<std::mutex> lock(source_nodes_mutex_);
            auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
                [sourceNodeId](const std::shared_ptr<SourceNode>& node) {
                    return node && node->instanceId() == sourceNodeId;
                });

            if (it != source_nodes_.end()) {
                source_nodes_.erase(it);
                rebuildSourceNodeSnapshotLocked();
            }
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

        auto sharedNode = std::shared_ptr<AudioFileSourceNode>(std::move(newSourceNode));

        {
            std::lock_guard<std::mutex> lock(source_nodes_mutex_);
            auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
                [oldSourceNodeId](const std::shared_ptr<SourceNode>& node) {
                    return node && node->instanceId() == oldSourceNodeId;
                });

            if (it != source_nodes_.end()) {
                *it = sharedNode;
            } else {
                source_nodes_.push_back(sharedNode);
            }
            rebuildSourceNodeSnapshotLocked();
        }

        // Update the clip's source node reference
        clip->sourceNodeInstanceId = newSourceNodeId;

        return true;
    }

    bool TimelineTrack::addDeviceInputSource(std::unique_ptr<DeviceInputSourceNode> sourceNode) {
        if (!sourceNode)
            return false;

        auto sharedNode = std::shared_ptr<DeviceInputSourceNode>(std::move(sourceNode));
        {
            std::lock_guard<std::mutex> lock(source_nodes_mutex_);
            source_nodes_.push_back(sharedNode);
            rebuildSourceNodeSnapshotLocked();
        }
        return true;
    }

    bool TimelineTrack::removeSource(int32_t sourceId) {
        std::lock_guard<std::mutex> lock(source_nodes_mutex_);
        auto it = std::find_if(source_nodes_.begin(), source_nodes_.end(),
            [sourceId](const std::shared_ptr<SourceNode>& node) {
                return node && node->instanceId() == sourceId;
            });

        if (it == source_nodes_.end())
            return false;

        source_nodes_.erase(it);
        rebuildSourceNodeSnapshotLocked();
        return true;
    }

    std::shared_ptr<SourceNode> TimelineTrack::getSourceNode(int32_t instanceId) {
        return findSourceNode(instanceId);
    }

    void TimelineTrack::reconfigureBuffers(uint32_t channelCount, uint32_t bufferSizeInFrames) {
        // This should ONLY be called from non-audio thread (e.g., during initialization or config change)
        mixed_source_buffers_.clear();
        mixed_source_buffers_.resize(channelCount);

        for (auto& channelBuffer : mixed_source_buffers_) {
            channelBuffer.resize(bufferSizeInFrames);
        }

        // Update buffer pointers
        mixed_source_buffer_ptrs_.clear();
        mixed_source_buffer_ptrs_.reserve(channelCount);
        for (auto& channelBuffer : mixed_source_buffers_) {
            mixed_source_buffer_ptrs_.push_back(channelBuffer.data());
        }
    }

    void TimelineTrack::ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount) {
        // Defensive check: If buffers were pre-allocated correctly, this should be a no-op
        // Only resize if buffers are insufficient (shouldn't happen in normal operation)

        // Check if buffers are already adequate
        if (mixed_source_buffers_.size() == numChannels &&
            (numChannels == 0 || (!mixed_source_buffers_.empty() &&
             mixed_source_buffers_[0].size() >= static_cast<size_t>(frameCount)))) {
            // Buffers are already adequate - fast path (no allocation)
            return;
        }

        // If we get here, buffers weren't pre-allocated correctly
        // This shouldn't happen but we handle it defensively
        // WARNING: This allocates memory in the audio thread (not real-time safe!)
        // Use reconfigureBuffers() from non-audio thread to avoid this!
        try {
            // Only grow, never shrink (to avoid repeated allocations)
            const size_t targetFrames = std::max(
                mixed_source_buffers_.empty() ? 0 : mixed_source_buffers_[0].size(),
                static_cast<size_t>(frameCount)
            );

            mixed_source_buffers_.resize(numChannels);
            for (auto& channelBuffer : mixed_source_buffers_) {
                if (channelBuffer.size() < targetFrames) {
                    channelBuffer.resize(targetFrames);
                }
            }

            // Update buffer pointers
            mixed_source_buffer_ptrs_.clear();
            mixed_source_buffer_ptrs_.reserve(numChannels);
            for (auto& channelBuffer : mixed_source_buffers_) {
                mixed_source_buffer_ptrs_.push_back(channelBuffer.data());
            }
        } catch (const std::bad_alloc&) {
            // Memory allocation failed - clear buffers and fail gracefully
            // Audio will be silent for this callback but won't crash
            mixed_source_buffers_.clear();
            mixed_source_buffer_ptrs_.clear();
        }
    }

    std::shared_ptr<SourceNode> TimelineTrack::findSourceNode(int32_t instanceId) const {
        auto snapshot = std::atomic_load_explicit(&source_nodes_snapshot_, std::memory_order_acquire);
        if (!snapshot)
            return nullptr;

        auto it = std::find_if(snapshot->begin(), snapshot->end(),
            [instanceId](const std::shared_ptr<SourceNode>& node) {
                return node && node->instanceId() == instanceId;
            });

        if (it == snapshot->end())
            return nullptr;

        return *it;
    }

    void TimelineTrack::rebuildSourceNodeSnapshotLocked() {
        auto snapshot = std::make_shared<SourceNodeList>(source_nodes_);
        std::shared_ptr<const SourceNodeList> constSnapshot = snapshot;
        std::atomic_store_explicit(&source_nodes_snapshot_, constSnapshot, std::memory_order_release);
    }

    void TimelineTrack::processAudio(
        remidy::AudioProcessContext& process,
        const TimelineState& timeline
    ) {
        const int32_t frameCount = process.frameCount();
        const uint32_t numChannels = std::min(channel_count_,
            static_cast<uint32_t>(process.audioOutBusCount() > 0 ? process.outputChannelCount(0) : 0));

        if (numChannels == 0)
            return;

        // Ensure our temporary buffers are allocated
        ensureBuffersAllocated(numChannels, frameCount);

        // Clear mixed source buffers
        for (auto& channelBuffer : mixed_source_buffers_) {
            std::memset(channelBuffer.data(), 0, frameCount * sizeof(float));
        }

        // Step 1: Build clip map for absolute position calculations
        auto allClips = clip_manager_.getAllClips();
        std::unordered_map<int32_t, const ClipData*> clipMap;
        clipMap.reserve(allClips.size());
        for (auto& clip : allClips) {
            clipMap[clip.clipId] = &clip;
        }

        // Step 2: Query active clips at current timeline position
        auto activeClips = clip_manager_.getActiveClipsAt(timeline.playheadPosition);

        // Step 3: Process audio source nodes for each active clip
        for (const auto& clip : activeClips) {
            if (clip.clipType != ClipType::Audio)
                continue;  // Skip MIDI clips in this loop

            if (clip.muted)
                continue;

            // Find the source node for this clip
            auto sourceNode = findSourceNode(clip.sourceNodeInstanceId);
            if (!sourceNode || sourceNode->nodeType() != SourceNodeType::AudioFileSource)
                continue;

            auto* audioSourceNode = dynamic_cast<AudioSourceNode*>(sourceNode.get());
            if (!audioSourceNode)
                continue;

            // Calculate position within the source file using anchor-aware calculation
            int64_t sourcePosition = clip.getSourcePosition(timeline.playheadPosition, clipMap);
            if (sourcePosition < 0)
                continue;

            // Seek source node to correct position
            audioSourceNode->seek(sourcePosition);

            // Set playing state
            audioSourceNode->setPlaying(timeline.isPlaying);

            // Create temporary buffers for this source
            std::vector<std::vector<float>> tempBuffers(numChannels);
            std::vector<float*> tempBufferPtrs;
            tempBufferPtrs.reserve(numChannels);

            for (auto& buf : tempBuffers) {
                buf.resize(frameCount);
                tempBufferPtrs.push_back(buf.data());
            }

            // Process the source node
            audioSourceNode->processAudio(tempBufferPtrs.data(), numChannels, frameCount);

            // Mix into our mixed source buffer with gain applied
            for (uint32_t ch = 0; ch < numChannels; ++ch) {
                for (int32_t frame = 0; frame < frameCount; ++frame) {
                    mixed_source_buffers_[ch][frame] += tempBuffers[ch][frame] * static_cast<float>(clip.gain);
                }
            }
        }

        // Step 3.5: Process MIDI clips (NEW)
        for (const auto& clip : activeClips) {
            if (clip.clipType != ClipType::Midi)
                continue;  // Skip audio clips in this loop

            if (clip.muted)
                continue;

            // Find the MIDI source node for this clip
            auto sourceNode = findSourceNode(clip.sourceNodeInstanceId);
            if (!sourceNode || sourceNode->nodeType() != SourceNodeType::MidiClipSource)
                continue;

            auto* midiNode = dynamic_cast<MidiSourceNode*>(sourceNode.get());
            if (!midiNode)
                continue;

            // Calculate position within the source using anchor-aware calculation
            int64_t sourcePosition = clip.getSourcePosition(timeline.playheadPosition, clipMap);
            if (sourcePosition < 0)
                continue;

            // Seek MIDI source to correct position
            midiNode->seek(sourcePosition);
            midiNode->setPlaying(timeline.isPlaying);

            // Generate MIDI events into AudioProcessContext event_in
            midiNode->processEvents(
                process.eventIn(),
                frameCount,
                static_cast<int32_t>(sample_rate_),
                timeline.tempo
            );
        }

        // Step 4: Process device input sources
        // Get device input buffer info from AudioProcessContext
        const uint32_t deviceChannelCount =
            process.audioInBusCount() > 0 ? process.inputChannelCount(0) : 0;

        if (deviceChannelCount > 0) {
            auto snapshot = std::atomic_load_explicit(&source_nodes_snapshot_, std::memory_order_acquire);
            if (snapshot) {
                for (const auto& sourceNode : *snapshot) {
                    if (!sourceNode || sourceNode->nodeType() != SourceNodeType::DeviceInput)
                        continue;

                    auto* deviceInputNode = dynamic_cast<DeviceInputSourceNode*>(sourceNode.get());
                    if (deviceInputNode) {
                        // Get device input buffer pointers from AudioProcessContext
                        std::vector<float*> deviceInputPtrs;
                        deviceInputPtrs.reserve(deviceChannelCount);
                        for (uint32_t ch = 0; ch < deviceChannelCount; ++ch) {
                            deviceInputPtrs.push_back(const_cast<float*>(process.getFloatInBuffer(0, ch)));
                        }

                        deviceInputNode->setDeviceInputBuffers(deviceInputPtrs.data(), deviceChannelCount);
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
        }

        // Step 5: Write mixed source buffer to AudioProcessContext INPUT
        // Timeline track writes to sequencer track's input, then plugins process input->output
        for (uint32_t ch = 0; ch < numChannels && ch < mixed_source_buffer_ptrs_.size(); ++ch) {
            float* inBuffer = process.getFloatInBuffer(0, ch);
            if (inBuffer) {
                std::memcpy(inBuffer, mixed_source_buffer_ptrs_[ch], frameCount * sizeof(float));
            }
        }
    }

} // namespace uapmd
