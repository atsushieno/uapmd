#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <remidy/remidy.hpp>
#include <uapmd-graph/uapmd-graph.hpp>
#include <uapmd-data/uapmd-data.hpp>
#include "LatencyCompensationTypes.hpp"

namespace uapmd {

// The undoable edits a project supports, in one place.
//
// Every method here records one history step and applies it, subject to the
// origin: a User or Remote edit enters history, while Load, UndoRedo and
// Internal changes are applied without being recorded. Callers do not open
// history scopes for a single edit -- that is what this does.
//
// Use TimelineFacade for reading the document and for changes that are not
// user edits; use this for anything a user could undo.
class ProjectCommands {
protected:
    ProjectCommands() = default;

public:
    virtual ~ProjectCommands() = default;

    ProjectCommands(const ProjectCommands&) = delete;
    ProjectCommands& operator=(const ProjectCommands&) = delete;

    // Clip properties. Each returns false when the clip does not exist or the
    // change was rejected; a change to the value already in place succeeds
    // without creating a history step.
    virtual bool setClipEnabled(int32_t trackIndex, int32_t clipId, bool enabled,
                                ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipAnchor(int32_t trackIndex, int32_t clipId, const TimeReference& anchor,
                               ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipGain(int32_t trackIndex, int32_t clipId, double gain,
                             ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipMuted(int32_t trackIndex, int32_t clipId, bool muted,
                              ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool resizeClip(int32_t trackIndex, int32_t clipId, int64_t newDurationSamples,
                            ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipName(int32_t trackIndex, int32_t clipId, const std::string& name,
                             ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipFilepath(int32_t trackIndex, int32_t clipId, const std::string& filepath,
                                 ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipNeedsFileSave(int32_t trackIndex, int32_t clipId, bool needsSave,
                                      ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipMarkers(int32_t trackIndex, int32_t clipId, std::vector<ClipMarker> markers,
                                ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setClipAudioWarps(int32_t trackIndex, int32_t clipId,
                                   std::vector<AudioWarpPoint> audioWarps,
                                   ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Track properties. These address their track by its stable document
    // identity during replay, so inserting or removing another track does not
    // redirect an undo to the wrong one.
    virtual bool setTrackGain(int32_t trackIndex, double gain,
                              ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackMuted(int32_t trackIndex, bool muted,
                               ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackSolo(int32_t trackIndex, bool solo,
                              ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackBypassed(int32_t trackIndex, bool bypassed,
                                  ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setTrackFreezePolicyEnabled(int32_t trackIndex, bool enabled,
                                             ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Plug-in properties. A plug-in is addressed by its document identity, not
    // its runtime instance id, which changes when it is removed and restored.
    virtual bool setPluginBypassed(int32_t instanceId, bool bypassed,
                                   ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setPluginParameterValue(int32_t instanceId, int32_t parameterIndex, double value,
                                         ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    // Per-note edits share the plug-in's identity and history, so changing the
    // selected key, channel or group does not silently bypass undo.
    virtual bool setPluginPerNoteControllerValue(
        int32_t instanceId,
        remidy::PerNoteControllerContextTypes contextType,
        remidy::PerNoteControllerContext context,
        int32_t parameterIndex,
        double value,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setPluginGroup(int32_t instanceId, uint8_t group,
                                ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Device inputs. Adding one, rerouting it and removing it all write the
    // same per-input value, so undoing an add is a removal without needing a
    // separate structural operation.
    virtual bool addDeviceInputToTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool setDeviceInputChannels(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool removeDeviceInputFromTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Track graph edges. Presence is the value here too, so undoing a
    // connect is a disconnect. `error` reports why the graph rejected the
    // change -- a cycle, a missing endpoint, a direction mismatch.
    virtual bool connectTrackGraph(
        int32_t trackIndex,
        const uapmd_graph::AudioPluginGraphConnection& connection,
        std::string& error,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;
    virtual bool disconnectTrackGraphConnection(
        int32_t trackIndex,
        int64_t connectionId,
        std::string& error,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Replaces a track's graph with a fresh one of the requested type. The
    // event buffer size is the caller's, not the engine's: the project loader
    // and the application model each supply their own.
    virtual bool replaceTrackGraphType(
        int32_t trackIndex,
        const std::string& graphTypeId,
        size_t eventBufferSizeInBytes,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Project-wide properties.
    //
    // Callers are responsible for validating marker identity and reference
    // cycles before submitting.
    virtual bool setMasterTrackMarkers(
        std::vector<ClipMarker> markers,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // The complete persisted latency/monitoring configuration, as one history
    // step. Callers that edit a single field copy
    // latencyCompensationManager()->projectSettings(), alter that field, and
    // submit the resulting snapshot.
    virtual bool setLatencyCompensationSettings(
        const LatencyCompensationProjectSettings& settings,
        ProjectMutationOrigin origin = ProjectMutationOrigin::User) = 0;

    // Groups several edits into one history step. Prefer the scoped forms in
    // uapmd-data over pairing these by hand.
    virtual ProjectCommandManager& history() = 0;
};

} // namespace uapmd
