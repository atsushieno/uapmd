#include "ProjectCommandsImpl.hpp"

// Every undoable project edit. Each one names its property descriptor and
// its address; the shared machinery does the rest.

namespace uapmd {

    using namespace timeline_detail;

    bool ProjectCommandsImpl::setClipEnabled(
        int32_t trackIndex, int32_t clipId, bool enabled, ProjectMutationOrigin origin) {
        return executeClip<ClipEnabledProperty>(trackIndex, clipId, enabled, origin);
    }

    bool ProjectCommandsImpl::setClipAnchor(
        int32_t trackIndex, int32_t clipId, const TimeReference& anchor, ProjectMutationOrigin origin) {
        return executeClip<ClipAnchorProperty>(trackIndex, clipId, anchor, origin);
    }

    bool ProjectCommandsImpl::setClipGain(
        int32_t trackIndex, int32_t clipId, double gain, ProjectMutationOrigin origin) {
        return executeClip<ClipGainProperty>(trackIndex, clipId, gain, origin);
    }

    bool ProjectCommandsImpl::setClipMuted(
        int32_t trackIndex, int32_t clipId, bool muted, ProjectMutationOrigin origin) {
        return executeClip<ClipMutedProperty>(trackIndex, clipId, muted, origin);
    }

    bool ProjectCommandsImpl::resizeClip(
        int32_t trackIndex, int32_t clipId, int64_t newDurationSamples, ProjectMutationOrigin origin) {
        return executeClip<ClipDurationProperty>(trackIndex, clipId, newDurationSamples, origin);
    }

    bool ProjectCommandsImpl::setClipName(
        int32_t trackIndex, int32_t clipId, const std::string& name, ProjectMutationOrigin origin) {
        return executeClip<ClipNameProperty>(trackIndex, clipId, name, origin);
    }

    bool ProjectCommandsImpl::setClipFilepath(
        int32_t trackIndex, int32_t clipId, const std::string& filepath, ProjectMutationOrigin origin) {
        return executeClip<ClipFilepathProperty>(trackIndex, clipId, filepath, origin);
    }

    bool ProjectCommandsImpl::setClipNeedsFileSave(
        int32_t trackIndex, int32_t clipId, bool needsSave, ProjectMutationOrigin origin) {
        return executeClip<ClipNeedsFileSaveProperty>(trackIndex, clipId, needsSave, origin);
    }

    bool ProjectCommandsImpl::setClipMarkers(
        int32_t trackIndex, int32_t clipId, std::vector<ClipMarker> markers, ProjectMutationOrigin origin) {
        return executeClip<ClipMarkersProperty>(trackIndex, clipId, std::move(markers), origin);
    }

    bool ProjectCommandsImpl::setClipAudioWarps(
        int32_t trackIndex, int32_t clipId, std::vector<AudioWarpPoint> audioWarps, ProjectMutationOrigin origin) {
        return executeClip<ClipAudioWarpsProperty>(trackIndex, clipId, std::move(audioWarps), origin);
    }

    bool ProjectCommandsImpl::setTrackGain(
        int32_t trackIndex, double gain, ProjectMutationOrigin origin) {
        return executeTrack<TrackGainProperty>(trackIndex, gain, origin);
    }

    bool ProjectCommandsImpl::setTrackMuted(
        int32_t trackIndex, bool muted, ProjectMutationOrigin origin) {
        return executeTrack<TrackMutedProperty>(trackIndex, muted, origin);
    }

    bool ProjectCommandsImpl::setTrackSolo(
        int32_t trackIndex, bool solo, ProjectMutationOrigin origin) {
        return executeTrack<TrackSoloProperty>(trackIndex, solo, origin);
    }

    bool ProjectCommandsImpl::setTrackBypassed(
        int32_t trackIndex, bool bypassed, ProjectMutationOrigin origin) {
        return executeTrack<TrackBypassedProperty>(trackIndex, bypassed, origin);
    }

    bool ProjectCommandsImpl::setTrackFreezePolicyEnabled(
        int32_t trackIndex, bool enabled, ProjectMutationOrigin origin) {
        // The master track has no freeze policy.
        if (trackIndex == kMasterTrackIndex)
            return false;
        return executeTrack<TrackFreezePolicyProperty>(trackIndex, enabled, origin);
    }

    bool ProjectCommandsImpl::setPluginBypassed(
        int32_t instanceId, bool bypassed, ProjectMutationOrigin origin) {
        return executePlugin<PluginBypassedProperty>(instanceId, bypassed, origin);
    }

    bool ProjectCommandsImpl::setPluginParameterValue(
        int32_t instanceId, int32_t parameterIndex, double value, ProjectMutationOrigin origin) {
        auto plugin = target_.addresses().pluginAddress(instanceId);
        if (!plugin)
            return false;
        return execute<PluginParameterProperty>(
            PluginParameterAddress{
                .plugin = std::move(*plugin),
                .parameterIndex = parameterIndex
            },
            value,
            origin);
    }

    bool ProjectCommandsImpl::setPluginPerNoteControllerValue(
        int32_t instanceId,
        remidy::PerNoteControllerContextTypes contextType,
        remidy::PerNoteControllerContext context,
        int32_t parameterIndex,
        double value,
        ProjectMutationOrigin origin) {
        auto plugin = target_.addresses().pluginAddress(instanceId);
        if (!plugin)
            return false;
        return execute<PluginPerNoteProperty>(
            PluginPerNoteAddress{
                .plugin = std::move(*plugin),
                .contextType = contextType,
                .context = context,
                .parameterIndex = parameterIndex
            },
            value,
            origin);
    }

    bool ProjectCommandsImpl::setPluginGroup(
        int32_t instanceId, uint8_t group, ProjectMutationOrigin origin) {
        // 0xFF reports "no group", which is not a value the user can set.
        if (engine_.getInstanceGroup(instanceId) == 0xFF)
            return false;
        return executePlugin<PluginGroupProperty>(instanceId, group, origin);
    }

    bool ProjectCommandsImpl::setMasterTrackMarkers(
        std::vector<ClipMarker> markers, ProjectMutationOrigin origin) {
        return execute<MasterTrackMarkersProperty>(
            std::monostate{}, std::move(markers), origin);
    }

} // namespace uapmd
