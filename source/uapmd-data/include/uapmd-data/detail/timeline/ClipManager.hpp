#pragma once

#include "TimelineTypes.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace uapmd {

    // Manages clips for a single track
    // Thread-safe for concurrent UI and RT thread access
    class ClipManager {
    public:
        // Immutable snapshot of all clips — built on UI thread, read lock-free on RT thread
        struct ClipSnapshot {
            std::vector<ClipData> clips;
            std::unordered_map<int32_t, const ClipData*> clipMap; // pointers into clips
            std::unordered_map<std::string, const ClipData*> clipReferenceMap; // pointers into clips
        };

        explicit ClipManager(std::string referencePrefix = {})
            : reference_prefix_(std::move(referencePrefix)) {}
        ~ClipManager() = default;

        // Clip CRUD operations (UI thread)
        int32_t addClip(const ClipData& clip);
        bool removeClip(int32_t clipId);
        ClipData* getClip(int32_t clipId);
        const ClipData* getClip(int32_t clipId) const;
        std::vector<ClipData> getAllClips();
        std::vector<ClipData> getAllClips() const;

        // Clip modifications (UI thread)
        bool moveClip(int32_t clipId, const TimelinePosition& newPosition);
        bool resizeClip(int32_t clipId, int64_t newDuration);
        bool setClipGain(int32_t clipId, double gain);
        bool setClipMuted(int32_t clipId, bool muted);
        bool setClipName(int32_t clipId, const std::string& name);
        bool setClipFilepath(int32_t clipId, const std::string& filepath);
        bool setClipNeedsFileSave(int32_t clipId, bool needsSave);
        bool clipNeedsFileSave(int32_t clipId) const;
        bool setClipAnchor(int32_t clipId, const TimeReference& anchor, int32_t sampleRate);
        bool setClipPosition(int32_t clipId, const TimelinePosition& position);
        bool setClipMarkers(int32_t clipId, std::vector<ClipMarker> markers);
        bool setAudioWarps(int32_t clipId, std::vector<AudioWarpPoint> audioWarps);

        // Query clips at timeline position (RT-safe after initial query)
        std::vector<ClipData> getActiveClipsAt(const TimelinePosition& position);
        std::vector<ClipData> getActiveClipsAt(const TimelinePosition& position) const;

        // Clear all clips
        void clearAll();

        // Get number of clips
        size_t clipCount() const;

        // RT-safe: returns immutable snapshot without locking (atomic_load)
        std::shared_ptr<const ClipSnapshot> getSnapshotRT() const;

    private:
        mutable std::mutex clips_mutex_;  // Protects clips_ map
        std::unordered_map<int32_t, ClipData> clips_;
        int32_t next_clip_id_{1};  // Start from 1, 0 is invalid
        std::string reference_prefix_;

        // Helper to generate unique clip IDs
        int32_t generateClipId();

        // Atomic snapshot for RT-safe access (rebuilt on every mutation under clips_mutex_)
        std::shared_ptr<const ClipSnapshot> clip_snapshot_;
        void rebuildSnapshotLocked();
    };

} // namespace uapmd
