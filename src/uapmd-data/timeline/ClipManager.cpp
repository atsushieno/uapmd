#include <algorithm>
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    int32_t ClipManager::generateClipId() {
        return next_clip_id_++;
    }

    int32_t ClipManager::addClip(const ClipData& clip) {
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

    bool ClipManager::removeClip(int32_t clipId) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        clips_.erase(it);
        return true;
    }

    ClipData* ClipManager::getClip(int32_t clipId) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return nullptr;
        return &it->second;
    }

    const ClipData* ClipManager::getClip(int32_t clipId) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return nullptr;
        return &it->second;
    }

    namespace {
        template <typename ClipsMap>
        std::vector<ClipData> collectClipCopies(const ClipsMap& source) {
            std::vector<ClipData> result;
            result.reserve(source.size());
            for (const auto& pair : source) {
                result.push_back(pair.second);
            }
            return result;
        }

        template <typename ClipsMap>
        std::vector<ClipData> collectActiveClipCopies(const ClipsMap& source, const TimelinePosition& position) {
            if (source.empty())
                return {};

            std::vector<ClipData> clipCopies;
            clipCopies.reserve(source.size());
            for (const auto& pair : source) {
                clipCopies.push_back(pair.second);
            }

            std::unordered_map<int32_t, const ClipData*> clipMap;
            clipMap.reserve(clipCopies.size());
            for (auto& clip : clipCopies) {
                clipMap[clip.clipId] = &clip;
            }

            std::vector<const ClipData*> activeClips;
            activeClips.reserve(clipCopies.size());
            for (auto& clip : clipCopies) {
                TimelinePosition absPos = clip.getAbsolutePosition(clipMap);
                if (position.samples >= absPos.samples &&
                    position.samples < absPos.samples + clip.durationSamples) {
                    activeClips.push_back(&clip);
                }
            }

            std::sort(activeClips.begin(), activeClips.end(), [&clipMap](const ClipData* a, const ClipData* b) {
                TimelinePosition aPosAbs = a->getAbsolutePosition(clipMap);
                TimelinePosition bPosAbs = b->getAbsolutePosition(clipMap);
                return aPosAbs.samples < bPosAbs.samples;
            });

            std::vector<ClipData> result;
            result.reserve(activeClips.size());
            for (const auto* clipPtr : activeClips) {
                result.push_back(*clipPtr);
            }

            return result;
        }
    } // namespace

    std::vector<ClipData> ClipManager::getAllClips() {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return collectClipCopies(clips_);
    }

    std::vector<ClipData> ClipManager::getAllClips() const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return collectClipCopies(clips_);
    }

    bool ClipManager::moveClip(int32_t clipId, const TimelinePosition& newPosition) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.position = newPosition;
        return true;
    }

    bool ClipManager::resizeClip(int32_t clipId, int64_t newDuration) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.durationSamples = newDuration;
        return true;
    }

    bool ClipManager::setClipGain(int32_t clipId, double gain) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.gain = gain;
        return true;
    }

    bool ClipManager::setClipMuted(int32_t clipId, bool muted) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.muted = muted;
        return true;
    }

    bool ClipManager::setClipName(int32_t clipId, const std::string& name) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.name = name;
        return true;
    }

    bool ClipManager::setClipFilepath(int32_t clipId, const std::string& filepath) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.filepath = filepath;
        return true;
    }

    bool ClipManager::setClipAnchor(int32_t clipId, int32_t anchorClipId, AnchorOrigin anchorOrigin, const TimelinePosition& anchorOffset) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.anchorClipId = anchorClipId;
        it->second.anchorOrigin = anchorOrigin;
        it->second.anchorOffset = anchorOffset;
        return true;
    }

    std::vector<ClipData> ClipManager::getActiveClipsAt(const TimelinePosition& position) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return collectActiveClipCopies(clips_, position);
    }

    std::vector<ClipData> ClipManager::getActiveClipsAt(const TimelinePosition& position) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return collectActiveClipCopies(clips_, position);
    }

    void ClipManager::clearAll() {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        clips_.clear();
    }

    size_t ClipManager::clipCount() const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return clips_.size();
    }

} // namespace uapmd
