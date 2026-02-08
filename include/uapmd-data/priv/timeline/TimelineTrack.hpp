#pragma once

#include <memory>
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
        explicit TimelineTrack(uint32_t channelCount, double sampleRate);
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
        SourceNode* getSourceNode(int32_t instanceId);

        // Timeline-aware processing
        // Writes mixed audio to AudioProcessContext output buffers
        void processAudio(
            remidy::AudioProcessContext& process,
            const TimelineState& timeline
        );

        // Channel information
        uint32_t channelCount() const { return channel_count_; }
        double sampleRate() const { return sample_rate_; }

    private:
        uint32_t channel_count_;
        double sample_rate_;

        ClipManager clip_manager_;
        std::vector<std::unique_ptr<SourceNode>> source_nodes_;  // Changed to SourceNode for polymorphism

        // Temporary buffers for mixing sources
        std::vector<std::vector<float>> mixed_source_buffers_;  // [channel][samples]
        std::vector<float*> mixed_source_buffer_ptrs_;  // Pointers to buffer channels

        // Helper to ensure buffers are allocated
        void ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount);

        // Helper to find source node by instance ID
        SourceNode* findSourceNode(int32_t instanceId);
    };

} // namespace uapmd
