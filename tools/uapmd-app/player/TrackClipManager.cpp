#include "TrackClipManager.hpp"
#include <algorithm>

namespace uapmd_app {

    int32_t TrackClipManager::generateClipId() {
        return next_clip_id_++;
    }

    int32_t TrackClipManager::addClip(const ClipData& clip) {
        std::lock_guard<std::mutex> lock(clips_mutex_);

        ClipData newClip = clip;

        // Generate ID if not provided
        if (newClip.clipId <= 0) {
            newClip.clipId = generateClipId();
        } else {
            // Ensure next_clip_id_ is always higher than any existing ID
            next_clip_id_ = std::max(next_clip_id_, newClip.clipId + 1);
        }

        clips_[newClip.clipId] = newClip;
        return newClip.clipId;
    }

    bool TrackClipManager::removeClip(int32_t clipId) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        clips_.erase(it);
        return true;
    }

    ClipData* TrackClipManager::getClip(int32_t clipId) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return nullptr;
        return &it->second;
    }

    const ClipData* TrackClipManager::getClip(int32_t clipId) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return nullptr;
        return &it->second;
    }

    std::vector<ClipData*> TrackClipManager::getAllClips() {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<ClipData*> result;
        result.reserve(clips_.size());
        for (auto& pair : clips_) {
            result.push_back(&pair.second);
        }
        return result;
    }

    std::vector<const ClipData*> TrackClipManager::getAllClips() const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<const ClipData*> result;
        result.reserve(clips_.size());
        for (const auto& pair : clips_) {
            result.push_back(&pair.second);
        }
        return result;
    }

    bool TrackClipManager::moveClip(int32_t clipId, const TimelinePosition& newPosition) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.position = newPosition;
        return true;
    }

    bool TrackClipManager::resizeClip(int32_t clipId, int64_t newDuration) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.durationSamples = newDuration;
        return true;
    }

    bool TrackClipManager::setClipGain(int32_t clipId, double gain) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.gain = gain;
        return true;
    }

    bool TrackClipManager::setClipMuted(int32_t clipId, bool muted) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.muted = muted;
        return true;
    }

    std::vector<ClipData*> TrackClipManager::getActiveClipsAt(const TimelinePosition& position) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<ClipData*> result;

        for (auto& pair : clips_) {
            if (pair.second.contains(position)) {
                result.push_back(&pair.second);
            }
        }

        // Sort by position (earlier clips first)
        std::sort(result.begin(), result.end(), [](const ClipData* a, const ClipData* b) {
            return a->position.samples < b->position.samples;
        });

        return result;
    }

    std::vector<const ClipData*> TrackClipManager::getActiveClipsAt(const TimelinePosition& position) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<const ClipData*> result;

        for (const auto& pair : clips_) {
            if (pair.second.contains(position)) {
                result.push_back(&pair.second);
            }
        }

        // Sort by position (earlier clips first)
        std::sort(result.begin(), result.end(), [](const ClipData* a, const ClipData* b) {
            return a->position.samples < b->position.samples;
        });

        return result;
    }

    void TrackClipManager::clearAll() {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        clips_.clear();
    }

    size_t TrackClipManager::clipCount() const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return clips_.size();
    }

} // namespace uapmd_app
