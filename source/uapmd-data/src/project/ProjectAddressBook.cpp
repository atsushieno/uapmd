#include "uapmd-data/uapmd-data.hpp"

namespace uapmd {
    ProjectAddressBook::ProjectAddressBook(
            const std::vector<std::shared_ptr<TimelineTrack>>& tracks,
            const std::shared_ptr<TimelineTrack>& masterTrack)
        : tracks_(tracks), master_track_(masterTrack) {
    }

    TimelineTrack* ProjectAddressBook::timelineTrack(std::string_view trackReferenceId) const {
        if (master_track_ && master_track_->referenceId() == trackReferenceId)
            return master_track_.get();
        for (const auto& track : tracks_)
            if (track && track->referenceId() == trackReferenceId)
                return track.get();
        return nullptr;
    }

    int32_t ProjectAddressBook::trackIndex(std::string_view trackReferenceId) const {
        if (master_track_ && master_track_->referenceId() == trackReferenceId)
            return kMasterTrackIndex;
        for (size_t index = 0; index < tracks_.size(); ++index)
            if (tracks_[index] && tracks_[index]->referenceId() == trackReferenceId)
                return static_cast<int32_t>(index);
        return -1;
    }

    int32_t ProjectAddressBook::clipId(const ClipAddress& address) const {
        const auto* track = timelineTrack(address.trackReferenceId);
        if (track)
            for (const auto& clip : track->clipManager().getAllClips())
                if (clip.referenceId == address.clipReferenceId)
                    return clip.clipId;
        return -1;
    }

    std::optional<ProjectObjectId> ProjectAddressBook::trackReferenceId(int32_t index) const {
        const auto* track = index == kMasterTrackIndex ? master_track_.get()
            : index >= 0 && static_cast<size_t>(index) < tracks_.size()
                ? tracks_[static_cast<size_t>(index)].get() : nullptr;
        if (!track)
            return std::nullopt;
        return track->referenceId();
    }

    std::optional<ClipAddress> ProjectAddressBook::clipAddress(int32_t index, int32_t clipIdentifier) const {
        const auto trackId = trackReferenceId(index);
        const auto* track = trackId ? timelineTrack(*trackId) : nullptr;
        const auto* clip = track ? track->clipManager().getClip(clipIdentifier) : nullptr;
        if (!clip)
            return std::nullopt;
        return ClipAddress{*trackId, clip->referenceId};
    }
}
