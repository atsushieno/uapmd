#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include "uapmd-data/uapmd-data.hpp"

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

    bool TimelineTrack::replaceClipSourceNode(int32_t clipId, std::unique_ptr<MidiSourceNode> newSourceNode) {
        if (!newSourceNode)
            return false;

        auto* clip = clip_manager_.getClip(clipId);
        if (!clip)
            return false;

        int32_t oldSourceNodeId = clip->sourceNodeInstanceId;
        int32_t newSourceNodeId = newSourceNode->instanceId();

        auto sharedNode = std::shared_ptr<MidiSourceNode>(std::move(newSourceNode));

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
        for (auto& channelBuffer : mixed_source_buffers_)
            channelBuffer.resize(bufferSizeInFrames);

        mixed_source_buffer_ptrs_.clear();
        mixed_source_buffer_ptrs_.reserve(channelCount);
        for (auto& channelBuffer : mixed_source_buffers_)
            mixed_source_buffer_ptrs_.push_back(channelBuffer.data());

        // Also allocate scratch buffers for per-source processing
        temp_source_buffers_.clear();
        temp_source_buffers_.resize(channelCount);
        for (auto& channelBuffer : temp_source_buffers_)
            channelBuffer.resize(bufferSizeInFrames);

        temp_source_buffer_ptrs_.clear();
        temp_source_buffer_ptrs_.reserve(channelCount);
        for (auto& channelBuffer : temp_source_buffers_)
            temp_source_buffer_ptrs_.push_back(channelBuffer.data());
    }

    void TimelineTrack::ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount) {
        // Defensive check: If buffers were pre-allocated correctly, this should be a no-op
        // Only resize if buffers are insufficient (shouldn't happen in normal operation)
        const auto needed = static_cast<size_t>(frameCount);

        auto isAdequate = [&](const std::vector<std::vector<float>>& bufs) {
            return bufs.size() == numChannels &&
                   (numChannels == 0 || bufs[0].size() >= needed);
        };

        if (isAdequate(mixed_source_buffers_) && isAdequate(temp_source_buffers_))
            return;

        // WARNING: This allocates memory in the audio thread (not real-time safe!)
        // Use reconfigureBuffers() from non-audio thread to avoid this!
        try {
            auto growBuffers = [&](std::vector<std::vector<float>>& bufs,
                                   std::vector<float*>& ptrs) {
                const size_t targetFrames = std::max(
                    bufs.empty() ? size_t{0} : bufs[0].size(), needed);
                bufs.resize(numChannels);
                for (auto& ch : bufs)
                    if (ch.size() < targetFrames)
                        ch.resize(targetFrames);
                ptrs.clear();
                ptrs.reserve(numChannels);
                for (auto& ch : bufs)
                    ptrs.push_back(ch.data());
            };
            growBuffers(mixed_source_buffers_, mixed_source_buffer_ptrs_);
            growBuffers(temp_source_buffers_, temp_source_buffer_ptrs_);
        } catch (const std::bad_alloc&) {
            mixed_source_buffers_.clear();
            mixed_source_buffer_ptrs_.clear();
            temp_source_buffers_.clear();
            temp_source_buffer_ptrs_.clear();
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
        for (uint32_t ch = 0; ch < numChannels && ch < mixed_source_buffers_.size(); ++ch)
            std::memset(mixed_source_buffers_[ch].data(), 0, frameCount * sizeof(float));

        // Get RT-safe clip snapshot (lock-free atomic load)
        auto clipSnapshot = clip_manager_.getSnapshotRT();

        if (clipSnapshot) {
            const auto& clips = clipSnapshot->clips;
            const auto& clipMap = clipSnapshot->clipMap;

            // Process audio source nodes — iterate all clips, skip inactive ones inline
            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Audio || clip.muted)
                    continue;

                // Check if clip is active at current playhead (anchor-aware)
                TimelinePosition absPos = clip.getAbsolutePosition(clipMap);
                if (timeline.playheadPosition.samples < absPos.samples ||
                    timeline.playheadPosition.samples >= absPos.samples + clip.durationSamples)
                    continue;

                auto sourceNode = findSourceNode(clip.sourceNodeInstanceId);
                if (!sourceNode || sourceNode->nodeType() != SourceNodeType::AudioFileSource)
                    continue;

                auto* audioSourceNode = dynamic_cast<AudioSourceNode*>(sourceNode.get());
                if (!audioSourceNode)
                    continue;

                int64_t sourcePosition = timeline.playheadPosition.samples - absPos.samples;
                if (sourcePosition < 0)
                    continue;

                audioSourceNode->seek(sourcePosition);
                audioSourceNode->setPlaying(timeline.isPlaying);

                // Zero pre-allocated scratch buffers and process
                for (uint32_t ch = 0; ch < numChannels && ch < temp_source_buffers_.size(); ++ch)
                    std::memset(temp_source_buffers_[ch].data(), 0, frameCount * sizeof(float));

                audioSourceNode->processAudio(temp_source_buffer_ptrs_.data(), numChannels, frameCount);

                // Mix into mixed source buffer with gain
                const float gain = static_cast<float>(clip.gain);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    for (int32_t frame = 0; frame < frameCount; ++frame)
                        mixed_source_buffers_[ch][frame] += temp_source_buffers_[ch][frame] * gain;
            }

            // Process MIDI clips
            for (const auto& clip : clips) {
                if (clip.clipType != ClipType::Midi || clip.muted)
                    continue;

                TimelinePosition absPos = clip.getAbsolutePosition(clipMap);
                if (timeline.playheadPosition.samples < absPos.samples ||
                    timeline.playheadPosition.samples >= absPos.samples + clip.durationSamples)
                    continue;

                auto sourceNode = findSourceNode(clip.sourceNodeInstanceId);
                if (!sourceNode || sourceNode->nodeType() != SourceNodeType::MidiClipSource)
                    continue;

                auto* midiNode = dynamic_cast<MidiSourceNode*>(sourceNode.get());
                if (!midiNode)
                    continue;

                int64_t sourcePosition = timeline.playheadPosition.samples - absPos.samples;
                if (sourcePosition < 0)
                    continue;

                midiNode->seek(sourcePosition);
                midiNode->setPlaying(timeline.isPlaying);

                midiNode->processEvents(
                    process.eventIn(),
                    frameCount,
                    static_cast<int32_t>(sample_rate_),
                    timeline.tempo
                );
            }
        }

        // Process device input sources
        const uint32_t deviceChannelCount =
            process.audioInBusCount() > 0 ? process.inputChannelCount(0) : 0;

        if (deviceChannelCount > 0) {
            auto srcSnapshot = std::atomic_load_explicit(&source_nodes_snapshot_, std::memory_order_acquire);
            if (srcSnapshot) {
                for (const auto& sourceNode : *srcSnapshot) {
                    if (!sourceNode || sourceNode->nodeType() != SourceNodeType::DeviceInput)
                        continue;

                    auto* deviceInputNode = dynamic_cast<DeviceInputSourceNode*>(sourceNode.get());
                    if (!deviceInputNode)
                        continue;

                    // Stack-allocated pointer array (no heap alloc)
                    constexpr uint32_t kMaxDeviceChannels = 16;
                    float* devicePtrs[kMaxDeviceChannels];
                    const uint32_t usableChannels = std::min(deviceChannelCount, kMaxDeviceChannels);
                    for (uint32_t ch = 0; ch < usableChannels; ++ch)
                        devicePtrs[ch] = const_cast<float*>(process.getFloatInBuffer(0, ch));

                    deviceInputNode->setDeviceInputBuffers(devicePtrs, usableChannels);
                    deviceInputNode->setPlaying(timeline.isPlaying);

                    // Zero and reuse pre-allocated scratch buffers
                    for (uint32_t ch = 0; ch < numChannels && ch < temp_source_buffers_.size(); ++ch)
                        std::memset(temp_source_buffers_[ch].data(), 0, frameCount * sizeof(float));

                    deviceInputNode->processAudio(temp_source_buffer_ptrs_.data(), numChannels, frameCount);

                    for (uint32_t ch = 0; ch < numChannels; ++ch)
                        for (int32_t frame = 0; frame < frameCount; ++frame)
                            mixed_source_buffers_[ch][frame] += temp_source_buffers_[ch][frame];
                }
            }
        }

        // Write mixed source buffer to AudioProcessContext INPUT
        // Timeline track writes to sequencer track's input, then plugins process input->output
        for (uint32_t ch = 0; ch < numChannels && ch < mixed_source_buffer_ptrs_.size(); ++ch) {
            float* inBuffer = process.getFloatInBuffer(0, ch);
            if (inBuffer)
                std::memcpy(inBuffer, mixed_source_buffer_ptrs_[ch], frameCount * sizeof(float));
        }
    }

} // namespace uapmd
