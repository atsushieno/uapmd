#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "TimelineTypes.hpp"
#include "ClipManager.hpp"
#include "SourceNode.hpp"
#include "AudioSourceNode.hpp"
#include "AudioFileSourceNode.hpp"
#include "DeviceInputSourceNode.hpp"
#include "MidiSourceNode.hpp"

namespace remidy { class AudioProcessContext; }

namespace uapmd {

    // Timeline track wrapper
    // Manages clips and sources, mixes audio into provided buffers
    // Independent of SequencerEngine for maximum reusability
    class TimelineTrack {
    public:
        explicit TimelineTrack(uint32_t channelCount, double sampleRate, uint32_t bufferSizeInFrames);
        ~TimelineTrack() = default;

        // Clip management
        ClipManager& clipManager() { return clip_manager_; }
        const ClipManager& clipManager() const { return clip_manager_; }

        int32_t addClip(const ClipData& clip, std::unique_ptr<AudioFileSourceNode> sourceNode);
        int32_t addClip(const ClipData& clip, std::unique_ptr<MidiSourceNode> sourceNode);  // NEW
        bool removeClip(int32_t clipId);
        bool replaceClipSourceNode(int32_t clipId, std::unique_ptr<AudioFileSourceNode> newSourceNode);

        // Source node management
        bool addDeviceInputSource(std::unique_ptr<DeviceInputSourceNode> sourceNode);
        bool removeSource(int32_t sourceId);
        std::shared_ptr<SourceNode> getSourceNode(int32_t instanceId);

        // Timeline-aware processing
        // Writes mixed audio to AudioProcessContext output buffers
        void processAudio(
            remidy::AudioProcessContext& process,
            const TimelineState& timeline
        );

        // Channel information
        uint32_t channelCount() const { return channel_count_; }
        double sampleRate() const { return sample_rate_; }

        // Configuration changes (call from non-audio thread only!)
        // Pre-allocates buffers to avoid real-time allocations during processAudio
        void reconfigureBuffers(uint32_t channelCount, uint32_t bufferSizeInFrames);

    private:
        uint32_t channel_count_;
        double sample_rate_;

        ClipManager clip_manager_;
        using SourceNodeList = std::vector<std::shared_ptr<SourceNode>>;
        SourceNodeList source_nodes_;
        mutable std::mutex source_nodes_mutex_;
        std::shared_ptr<const SourceNodeList> source_nodes_snapshot_;

        // Temporary buffers for mixing sources
        std::vector<std::vector<float>> mixed_source_buffers_;  // [channel][samples]
        std::vector<float*> mixed_source_buffer_ptrs_;  // Pointers to buffer channels

        // Helper to ensure buffers are allocated
        void ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount);

        // Source node snapshot helpers
        void rebuildSourceNodeSnapshotLocked();
        std::shared_ptr<SourceNode> findSourceNode(int32_t instanceId) const;
    };

} // namespace uapmd
