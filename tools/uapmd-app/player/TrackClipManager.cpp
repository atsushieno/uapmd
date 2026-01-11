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

    bool TrackClipManager::setClipName(int32_t clipId, const std::string& name) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.name = name;
        return true;
    }

    bool TrackClipManager::setClipFilepath(int32_t clipId, const std::string& filepath) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.filepath = filepath;
        return true;
    }

    bool TrackClipManager::setClipAnchor(int32_t clipId, int32_t anchorClipId, AnchorOrigin anchorOrigin, const TimelinePosition& anchorOffset) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.anchorClipId = anchorClipId;
        it->second.anchorOrigin = anchorOrigin;
        it->second.anchorOffset = anchorOffset;
        return true;
    }

    std::vector<ClipData*> TrackClipManager::getActiveClipsAt(const TimelinePosition& position) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<ClipData*> result;

        // Build clip map for absolute position calculation
        std::unordered_map<int32_t, const ClipData*> clipMap;
        for (const auto& pair : clips_) {
            clipMap[pair.first] = &pair.second;
        }

        for (auto& pair : clips_) {
            ClipData& clip = pair.second;
            // Calculate absolute position for this clip
            TimelinePosition absPos = clip.getAbsolutePosition(clipMap);

            // Check if the given position falls within this clip's range
            if (position.samples >= absPos.samples &&
                position.samples < absPos.samples + clip.durationSamples) {
                result.push_back(&clip);
            }
        }

        // Sort by absolute position (earlier clips first)
        std::sort(result.begin(), result.end(), [&clipMap](const ClipData* a, const ClipData* b) {
            TimelinePosition aPosAbs = a->getAbsolutePosition(clipMap);
            TimelinePosition bPosAbs = b->getAbsolutePosition(clipMap);
            return aPosAbs.samples < bPosAbs.samples;
        });

        return result;
    }

    std::vector<const ClipData*> TrackClipManager::getActiveClipsAt(const TimelinePosition& position) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        std::vector<const ClipData*> result;

        // Build clip map for absolute position calculation
        std::unordered_map<int32_t, const ClipData*> clipMap;
        for (const auto& pair : clips_) {
            clipMap[pair.first] = &pair.second;
        }

        for (const auto& pair : clips_) {
            const ClipData& clip = pair.second;
            // Calculate absolute position for this clip
            TimelinePosition absPos = clip.getAbsolutePosition(clipMap);

            // Check if the given position falls within this clip's range
            if (position.samples >= absPos.samples &&
                position.samples < absPos.samples + clip.durationSamples) {
                result.push_back(&clip);
            }
        }

        // Sort by absolute position (earlier clips first)
        std::sort(result.begin(), result.end(), [&clipMap](const ClipData* a, const ClipData* b) {
            TimelinePosition aPosAbs = a->getAbsolutePosition(clipMap);
            TimelinePosition bPosAbs = b->getAbsolutePosition(clipMap);
            return aPosAbs.samples < bPosAbs.samples;
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
