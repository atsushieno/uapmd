#include <algorithm>
#include <atomic>
#include <format>
#include <unordered_set>
#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {

    namespace {
        std::string makeClipReferenceId(std::string_view referencePrefix, int32_t clipId) {
            if (!referencePrefix.empty())
                return std::format("{}::clip_{:08x}", referencePrefix, static_cast<uint32_t>(clipId));
            return std::format("clip_{:08x}", static_cast<uint32_t>(clipId));
        }

        bool wouldCreateLocalAnchorCycle(
            const std::unordered_map<int32_t, ClipData>& clips,
            int32_t targetClipId,
            std::string_view targetReferenceId,
            std::string_view newAnchorReferenceId
        ) {
            if (newAnchorReferenceId.empty())
                return false;
            if (targetReferenceId == newAnchorReferenceId)
                return true;

            std::unordered_map<std::string, const ClipData*> byReferenceId;
            byReferenceId.reserve(clips.size());
            for (const auto& [_, clip] : clips)
                byReferenceId.emplace(clip.referenceId, &clip);

            std::unordered_set<std::string> visited;
            std::string currentReferenceId(newAnchorReferenceId);
            while (!currentReferenceId.empty()) {
                if (!visited.insert(currentReferenceId).second)
                    return true;
                if (currentReferenceId == targetReferenceId)
                    return true;

                auto it = byReferenceId.find(currentReferenceId);
                if (it == byReferenceId.end())
                    return false;

                const auto* clip = it->second;
                if (!clip)
                    return false;

                if (clip->clipId == targetClipId)
                    currentReferenceId = std::string(newAnchorReferenceId);
                else
                    currentReferenceId = clip->anchorReferenceId;
            }
            return false;
        }
    } // namespace

    int32_t ClipManager::generateClipId() {
        return next_clip_id_++;
    }

    void ClipManager::rebuildSnapshotLocked() {
        auto snap = std::make_shared<ClipSnapshot>();
        snap->clips.reserve(clips_.size());
        for (const auto& pair : clips_)
            snap->clips.push_back(pair.second);
        snap->clipMap.reserve(snap->clips.size());
        snap->clipReferenceMap.reserve(snap->clips.size());
        for (auto& clip : snap->clips) {
            snap->clipMap[clip.clipId] = &clip;
            snap->clipReferenceMap[clip.referenceId] = &clip;
        }
        std::atomic_store_explicit(&clip_snapshot_,
            std::shared_ptr<const ClipSnapshot>(snap),
            std::memory_order_release);
    }

    std::shared_ptr<const ClipManager::ClipSnapshot> ClipManager::getSnapshotRT() const {
        return std::atomic_load_explicit(&clip_snapshot_, std::memory_order_acquire);
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
        if (newClip.referenceId.empty())
            newClip.referenceId = makeClipReferenceId(reference_prefix_, newClip.clipId);

        clips_[newClip.clipId] = newClip;
        rebuildSnapshotLocked();
        return newClip.clipId;
    }

    bool ClipManager::removeClip(int32_t clipId) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        clips_.erase(it);
        rebuildSnapshotLocked();
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

            std::vector<const ClipData*> activeClips;
            activeClips.reserve(clipCopies.size());
            for (auto& clip : clipCopies) {
                if (position.samples >= clip.position.samples &&
                    position.samples < clip.position.samples + clip.durationSamples) {
                    activeClips.push_back(&clip);
                }
            }

            std::sort(activeClips.begin(), activeClips.end(), [](const ClipData* a, const ClipData* b) {
                return a->position.samples < b->position.samples;
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
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::resizeClip(int32_t clipId, int64_t newDuration) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.durationSamples = newDuration;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipGain(int32_t clipId, double gain) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.gain = gain;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipMuted(int32_t clipId, bool muted) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.muted = muted;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipName(int32_t clipId, const std::string& name) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.name = name;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipFilepath(int32_t clipId, const std::string& filepath) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.filepath = filepath;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipNeedsFileSave(int32_t clipId, bool needsSave) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.needsFileSave = needsSave;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::clipNeedsFileSave(int32_t clipId) const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;
        return it->second.needsFileSave;
    }

    bool ClipManager::setClipAnchor(int32_t clipId, const std::string& anchorReferenceId, AnchorOrigin anchorOrigin, const TimelinePosition& anchorOffset) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        if (wouldCreateLocalAnchorCycle(clips_, clipId, it->second.referenceId, anchorReferenceId))
            return false;

        it->second.anchorReferenceId = anchorReferenceId;
        it->second.anchorOrigin = anchorOrigin;
        it->second.anchorOffset = anchorOffset;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipPosition(int32_t clipId, const TimelinePosition& position) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.position = position;
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setClipMarkers(int32_t clipId, std::vector<ClipMarker> markers) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.markers = std::move(markers);
        rebuildSnapshotLocked();
        return true;
    }

    bool ClipManager::setAudioWarps(int32_t clipId, std::vector<AudioWarpPoint> audioWarps) {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        auto it = clips_.find(clipId);
        if (it == clips_.end())
            return false;

        it->second.audioWarps = std::move(audioWarps);
        rebuildSnapshotLocked();
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
        rebuildSnapshotLocked();
    }

    size_t ClipManager::clipCount() const {
        std::lock_guard<std::mutex> lock(clips_mutex_);
        return clips_.size();
    }

} // namespace uapmd
