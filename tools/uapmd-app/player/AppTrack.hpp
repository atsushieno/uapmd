#pragma once

#include "TimelineTypes.hpp"
#include "TrackClipManager.hpp"
#include "AppSourceNode.hpp"
#include "AppAudioFileSourceNode.hpp"
#include "AppDeviceInputSourceNode.hpp"
#include "uapmd/uapmd.hpp"
#include <memory>
#include <vector>

namespace uapmd_app {

    // App-level track wrapper
    // Wraps a uapmd::AudioPluginTrack and adds timeline/clip functionality
    class AppTrack {
    public:
        explicit AppTrack(uapmd::AudioPluginTrack* uapmdTrack, int32_t sampleRate);
        ~AppTrack() = default;

        // Clip management
        TrackClipManager& clipManager() { return clip_manager_; }
        const TrackClipManager& clipManager() const { return clip_manager_; }

        int32_t addClip(const ClipData& clip, std::unique_ptr<AppAudioFileSourceNode> sourceNode);
        bool removeClip(int32_t clipId);
        bool replaceClipSourceNode(int32_t clipId, std::unique_ptr<AppAudioFileSourceNode> newSourceNode);

        // Source node management
        bool addDeviceInputSource(std::unique_ptr<AppDeviceInputSourceNode> sourceNode);
        bool removeSource(int32_t sourceId);

        // Timeline-aware processing
        void processAudioWithTimeline(
            const TimelineState& timeline,
            float** deviceInputBuffers,
            uint32_t deviceChannelCount,
            int32_t frameCount,
            remidy::AudioProcessContext* trackContext  // Context for accessing track buffers
        );

        // Access to underlying uapmd track
        uapmd::AudioPluginTrack* uapmdTrack() { return uapmd_track_; }
        const uapmd::AudioPluginTrack* uapmdTrack() const { return uapmd_track_; }

    private:
        uapmd::AudioPluginTrack* uapmd_track_;  // Wrapped uapmd track (not owned)
        int32_t sample_rate_;

        TrackClipManager clip_manager_;
        std::vector<std::unique_ptr<AppSourceNode>> source_nodes_;

        // Temporary buffers for mixing sources
        std::vector<std::vector<float>> mixed_source_buffers_;  // [channel][samples]
        std::vector<float*> mixed_source_buffer_ptrs_;  // Pointers to buffer channels

        // Helper to ensure buffers are allocated
        void ensureBuffersAllocated(uint32_t numChannels, int32_t frameCount);

        // Helper to find source node by instance ID
        AppSourceNode* findSourceNode(int32_t instanceId);
    };

} // namespace uapmd_app
