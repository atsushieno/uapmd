#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <uapmd-data/uapmd-data.hpp>

namespace uapmd {

    class TimelineTrack;
    class SequencerTrack;

    // Stable document identity of one clip.
    struct ClipAddress {
        ProjectObjectId trackReferenceId;
        ProjectObjectId clipReferenceId;
    };

    // Stable document identity of one plug-in node. A runtime instance id is
    // not usable in history: removing and restoring a plug-in produces a new
    // instance id for the same document object.
    struct PluginAddress {
        ProjectObjectId trackReferenceId;
        std::string nodeId;
    };

    // Translation between the persistent identities that a command payload
    // carries and the runtime indexes and pointers the engine works with.
    //
    // This exists because indexes shift. A history step recorded against
    // track 3 must still find the right track after an earlier track was
    // inserted or removed, so every replayable mutation addresses its target
    // by reference id and resolves it at the moment it runs.
    //
    // The current undo operations each open-code this resolution inside their
    // apply lambdas -- resolveTrackByReferenceId, clipIdForReferenceId,
    // trackIndexForPersistentId, resolvePluginInstanceId are called from
    // thirteen different places. Collecting them here is what lets a command
    // carry identities as plain data and leaves the lookup to one
    // implementation.
    //
    // Resolution and capture are deliberately symmetric: capture produces the
    // address a command stores, resolve turns it back into something the
    // engine can act on. Every command needs both halves -- capture when it is
    // built, resolve each time it runs.
    //
    // Not thread safe. Call on the model thread only.
    class ProjectAddressBook {
    protected:
        ProjectAddressBook() = default;

    public:
        virtual ~ProjectAddressBook() = default;

        ProjectAddressBook(const ProjectAddressBook&) = delete;
        ProjectAddressBook& operator=(const ProjectAddressBook&) = delete;

        // Resolution: persistent identity -> live object.
        //
        // Each returns nullptr, nullopt, or a negative index when the object
        // no longer exists, which is an ordinary outcome during replay rather
        // than an error at this layer -- the command turns it into a failure
        // result with a message naming what went missing.

        virtual TimelineTrack* timelineTrack(std::string_view trackReferenceId) = 0;
        virtual SequencerTrack* sequencerTrack(std::string_view trackReferenceId) = 0;
        // Returns kMasterTrackIndex for the master track, -1 when unknown.
        virtual int32_t trackIndex(std::string_view trackReferenceId) const = 0;
        virtual int32_t clipId(const ClipAddress& address) const = 0;
        virtual int32_t pluginInstanceId(const PluginAddress& address) = 0;

        // Capture: live object -> persistent identity.

        virtual std::optional<ProjectObjectId> trackReferenceId(
            int32_t trackIndex) const = 0;
        virtual std::optional<ClipAddress> clipAddress(
            int32_t trackIndex,
            int32_t clipId) const = 0;
        virtual std::optional<PluginAddress> pluginAddress(
            int32_t instanceId) = 0;
    };

} // namespace uapmd
