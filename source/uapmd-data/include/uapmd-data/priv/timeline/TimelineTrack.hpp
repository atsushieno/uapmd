#pragma once

#include <atomic>
#include <functional>
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
        bool replaceClipSourceNode(int32_t clipId, std::unique_ptr<MidiSourceNode> newSourceNode);

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

        // NRPN parameter mapping callback.
        // When set, MidiClipSourceNodes with nrpnToParameterMapping==true route Assignable
        // Controller events to this callback instead of forwarding them as raw UMP.
        // Must be called from non-audio thread before playback begins.
        using NrpnParameterCallback = std::function<void(uint8_t group, uint32_t parameterIndex, uint32_t rawValue, bool isRelative)>;
        void setNrpnParameterCallback(NrpnParameterCallback cb);

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

        // Scratch buffers for per-source processing (reused for each source node)
        std::vector<std::vector<float>> temp_source_buffers_;   // [channel][samples]
        std::vector<float*> temp_source_buffer_ptrs_;

        NrpnParameterCallback nrpn_parameter_callback_{};

        // Helper to ensure buffers are allocated
        void ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount);

        // Source node snapshot helpers
        void rebuildSourceNodeSnapshotLocked();
        std::shared_ptr<SourceNode> findSourceNode(int32_t instanceId) const;
    };

} // namespace uapmd
