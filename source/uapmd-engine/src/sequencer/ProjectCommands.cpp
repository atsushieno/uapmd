#include "ProjectCommandsImpl.hpp"
#include <cmath>

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

    bool ProjectCommandsImpl::trimClipStart(
        int32_t trackIndex, int32_t clipId, int64_t deltaSamples, ProjectMutationOrigin origin) {
        auto address = target_.addresses().clipAddress(trackIndex, clipId);
        if (!address)
            return false;
        auto subject = ClipExtentProperty::resolve(target_, *address);
        if (!subject)
            return false;
        const auto& clip = *subject->clip;
        auto extent = ClipExtentProperty::read(target_, *subject);

        // Never down to nothing. A MIDI clip can grow back past the start of its
        // content, which then moves along with it (see extendMidiClipStart), as
        // far as the timeline's start; an audio clip cannot start before its
        // source's first sample.
        const int64_t earliest = clip.clipType == ClipType::Midi
            ? std::min<int64_t>(0, -clip.position.samples)
            : -extent.sourceOffsetSamples;
        const int64_t delta = std::clamp<int64_t>(
            deltaSamples, earliest, std::max<int64_t>(0, extent.durationSamples - 1));
        const double sampleRate = target_.timelineSampleRate();
        if (sampleRate <= 0.0)
            return false;
        const double deltaSeconds = static_cast<double>(delta) / sampleRate;

        // The anchor offset is added to whatever the clip is anchored to, so
        // moving it moves the start by the same amount for every anchor kind.
        extent.anchor.offset += deltaSeconds;
        extent.sourceOffsetSamples += delta;
        extent.durationSamples -= delta;

        const auto isOwnStart = [&clip](AudioWarpReferenceType type, const std::string& referenceClipId) {
            return (type == AudioWarpReferenceType::ClipStart || type == AudioWarpReferenceType::Manual)
                && (referenceClipId.empty() || referenceClipId == clip.referenceId);
        };
        for (auto& marker : extent.markers)
            if (isOwnStart(marker.referenceType, marker.referenceClipId))
                marker.clipPositionOffset -= deltaSeconds;
        for (auto& warp : extent.audioWarps)
            if (isOwnStart(warp.referenceType, warp.referenceClipId))
                warp.clipPositionOffset -= deltaSeconds;

        // Past the start of its content, a MIDI clip's content is shifted along
        // so it starts at the new start: the new room is part of the content.
        if (clip.clipType == ClipType::Midi && extent.sourceOffsetSamples < 0)
            return extendMidiClipStart(trackIndex, clipId, clip, std::move(*address), std::move(extent), origin);
        return execute<ClipExtentProperty>(std::move(*address), std::move(extent), origin);
    }

    bool ProjectCommandsImpl::extendMidiClipStart(
        int32_t trackIndex,
        int32_t clipId,
        const ClipData& clip,
        ClipAddress address,
        ClipExtent extent,
        ProjectMutationOrigin origin) {
        auto& timeline = engine_.timeline();
        auto fragment = timeline.captureClipFragment(trackIndex, clipId);
        if (!fragment || !fragment->isMidi())
            return false;

        // How far the content has to move, in its own ticks: the beats between
        // where it starts now and where the clip is to start.
        const double sampleRate = target_.timelineSampleRate();
        const double contentStart =
            static_cast<double>(clip.position.samples - clip.sourceOffsetSamples) / sampleRate;
        const double newStart = contentStart + static_cast<double>(extent.sourceOffsetSamples) / sampleRate;
        const auto& tempoMap = timeline.masterTempoMap();
        const double leadBeats = tempoMap.hasTempoData()
            ? tempoMap.secondsToBeats(contentStart) - tempoMap.secondsToBeats(newStart)
            : (contentStart - newStart) * (clip.clipTempo > 0.0 ? clip.clipTempo : 120.0) / 60.0;
        const auto leadTicks = static_cast<uint64_t>(std::llround(
            std::max(0.0, leadBeats) * static_cast<double>(clip.tickResolution > 0 ? clip.tickResolution : 480)));
        auto ticks = fragment->umpTickTimestamps;
        for (auto& tick : ticks)
            tick += leadTicks;
        extent.sourceOffsetSamples = 0;

        // One undo step for both halves, unless a caller already has one open.
        const bool records = origin == ProjectMutationOrigin::User || origin == ProjectMutationOrigin::Remote;
        const bool ownsStep = records && !dispatch_.state().compoundOpen;
        if (ownsStep && !dispatch_.beginStep("Extend clip", origin).succeeded())
            return false;
        const bool done =
            timeline.replaceMidiClipContent(trackIndex, clipId, fragment->umpEvents, std::move(ticks), origin)
            && execute<ClipExtentProperty>(std::move(address), std::move(extent), origin);
        if (ownsStep) {
            if (done)
                dispatch_.endStep();
            else
                dispatch_.cancelStep();
        }
        return done;
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

    bool ProjectCommandsImpl::addDeviceInputToTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin) {
        return executeDeviceInput(
            trackIndex, sourceNodeId, channelIndices, origin, "Add device input");
    }

    bool ProjectCommandsImpl::setDeviceInputChannels(
        int32_t trackIndex,
        int32_t sourceNodeId,
        const std::vector<uint32_t>& channelIndices,
        ProjectMutationOrigin origin) {
        return executeDeviceInput(
            trackIndex, sourceNodeId, channelIndices, origin, "Change device input routing");
    }

    bool ProjectCommandsImpl::removeDeviceInputFromTrack(
        int32_t trackIndex,
        int32_t sourceNodeId,
        ProjectMutationOrigin origin) {
        return executeDeviceInput(
            trackIndex, sourceNodeId, std::nullopt, origin, "Remove device input");
    }

    bool ProjectCommandsImpl::connectTrackGraph(
        int32_t trackIndex,
        const uapmd_graph::AudioPluginGraphConnection& connection,
        std::string& error,
        ProjectMutationOrigin origin) {
        auto trackReferenceId = target_.addresses().trackReferenceId(trackIndex);
        if (!trackReferenceId) {
            error = "Track not found";
            return false;
        }
        auto result = executeReporting<timeline_detail::GraphConnectionPresentProperty>(
            timeline_detail::GraphConnectionAddress{std::move(*trackReferenceId), connection},
            true,
            origin);
        if (!result.succeeded())
            error = result.error;
        return result.succeeded();
    }

    bool ProjectCommandsImpl::disconnectTrackGraphConnection(
        int32_t trackIndex,
        int64_t connectionId,
        std::string& error,
        ProjectMutationOrigin origin) {
        auto trackReferenceId = target_.addresses().trackReferenceId(trackIndex);
        if (!trackReferenceId) {
            error = "Track not found";
            return false;
        }
        // The id is a runtime handle; history addresses the connection itself.
        auto connection = target_.graphConnectionById(trackIndex, connectionId);
        if (!connection) {
            error = "Connection not found";
            return false;
        }
        auto result = executeReporting<timeline_detail::GraphConnectionPresentProperty>(
            timeline_detail::GraphConnectionAddress{
                std::move(*trackReferenceId), std::move(*connection)},
            false,
            origin);
        if (!result.succeeded())
            error = result.error;
        return result.succeeded();
    }

    bool ProjectCommandsImpl::replaceTrackGraphType(
        int32_t trackIndex,
        const std::string& graphTypeId,
        size_t eventBufferSizeInBytes,
        ProjectMutationOrigin origin) {
        auto trackReferenceId = target_.addresses().trackReferenceId(trackIndex);
        if (!trackReferenceId)
            return false;
        timeline_detail::TrackGraphSnapshot requested;
        requested.graphType = graphTypeId;
        return dispatch_
            .executeSynchronously(
                std::make_shared<timeline_detail::TrackGraphTypeCommand>(
                    target_,
                    std::move(*trackReferenceId),
                    std::move(requested),
                    eventBufferSizeInBytes),
                origin)
            .succeeded();
    }

    bool ProjectCommandsImpl::setMasterTrackMarkers(
        std::vector<ClipMarker> markers, ProjectMutationOrigin origin) {
        return execute<MasterTrackMarkersProperty>(
            std::monostate{}, std::move(markers), origin);
    }

    bool ProjectCommandsImpl::setLatencyCompensationSettings(
        const LatencyCompensationProjectSettings& settings, ProjectMutationOrigin origin) {
        return execute<LatencyCompensationSettingsProperty>(
            std::monostate{}, settings, origin);
    }

} // namespace uapmd
