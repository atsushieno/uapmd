#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "ProjectAddresses.hpp"

namespace uapmd {
    class TimelineTrack;

    // Resolves stable document identities against the current authored tracks
    // and clips. Track indexes and numeric clip IDs may change during replay.
    // The referenced storage must outlive this address book. Model thread only.
    class ProjectAddressBook {
        const std::vector<std::shared_ptr<TimelineTrack>>& tracks_;
        const std::shared_ptr<TimelineTrack>& master_track_;

    public:
        ProjectAddressBook(
            const std::vector<std::shared_ptr<TimelineTrack>>& tracks,
            const std::shared_ptr<TimelineTrack>& masterTrack);
        ProjectAddressBook(const ProjectAddressBook&) = delete;
        ProjectAddressBook& operator=(const ProjectAddressBook&) = delete;

        // Missing objects resolve to nullptr, -1, or nullopt.
        TimelineTrack* timelineTrack(std::string_view trackReferenceId) const;
        // Returns kMasterTrackIndex for the master track, -1 when unknown.
        int32_t trackIndex(std::string_view trackReferenceId) const;
        int32_t clipId(const ClipAddress& address) const;

        std::optional<ProjectObjectId> trackReferenceId(int32_t trackIndex) const;
        std::optional<ClipAddress> clipAddress(int32_t trackIndex, int32_t clipId) const;
    };
}
