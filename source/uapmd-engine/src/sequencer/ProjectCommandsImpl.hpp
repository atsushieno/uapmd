#pragma once

// The concrete ProjectCommands. Every undoable edit is built here as a command
// value and handed to the dispatcher, which owns the origin policy, the
// document transaction and the history entry.
//
// Private to the timeline facade implementation.

#include "uapmd-engine/uapmd-engine.hpp"
#include "TimelineProperties.hpp"

namespace uapmd {

    class ProjectCommandsImpl final : public ProjectCommands {
        // The project this edits. Structural commands need the engine
        // directly; property commands reach the document through the target.
        SequencerEngine& engine_;
        timeline_detail::PropertyCommandTarget& target_;
        ProjectCommandManager& dispatch_;

    public:
        ProjectCommandsImpl(
            SequencerEngine& engine,
            timeline_detail::PropertyCommandTarget& target,
            ProjectCommandManager& dispatch)
            : engine_(engine)
            , target_(target)
            , dispatch_(dispatch) {
        }

        ProjectCommandManager& history() override {
            return dispatch_;
        }

        bool setClipEnabled(int32_t, int32_t, bool, ProjectMutationOrigin) override;
        bool setClipAnchor(int32_t, int32_t, const TimeReference&, ProjectMutationOrigin) override;
        bool setClipGain(int32_t, int32_t, double, ProjectMutationOrigin) override;
        bool setClipMuted(int32_t, int32_t, bool, ProjectMutationOrigin) override;
        bool resizeClip(int32_t, int32_t, int64_t, ProjectMutationOrigin) override;
        bool setClipName(int32_t, int32_t, const std::string&, ProjectMutationOrigin) override;
        bool setClipFilepath(int32_t, int32_t, const std::string&, ProjectMutationOrigin) override;
        bool setClipNeedsFileSave(int32_t, int32_t, bool, ProjectMutationOrigin) override;
        bool setClipMarkers(int32_t, int32_t, std::vector<ClipMarker>, ProjectMutationOrigin) override;
        bool setClipAudioWarps(int32_t, int32_t, std::vector<AudioWarpPoint>, ProjectMutationOrigin) override;

        bool setTrackGain(int32_t, double, ProjectMutationOrigin) override;
        bool setTrackMuted(int32_t, bool, ProjectMutationOrigin) override;
        bool setTrackSolo(int32_t, bool, ProjectMutationOrigin) override;
        bool setTrackBypassed(int32_t, bool, ProjectMutationOrigin) override;
        bool setTrackFreezePolicyEnabled(int32_t, bool, ProjectMutationOrigin) override;

        bool setPluginBypassed(int32_t, bool, ProjectMutationOrigin) override;
        bool setPluginParameterValue(int32_t, int32_t, double, ProjectMutationOrigin) override;
        bool setPluginPerNoteControllerValue(
            int32_t,
            remidy::PerNoteControllerContextTypes,
            remidy::PerNoteControllerContext,
            int32_t,
            double,
            ProjectMutationOrigin) override;
        bool setPluginGroup(int32_t, uint8_t, ProjectMutationOrigin) override;

        bool addDeviceInputToTrack(
            int32_t, int32_t, const std::vector<uint32_t>&, ProjectMutationOrigin) override;
        bool setDeviceInputChannels(
            int32_t, int32_t, const std::vector<uint32_t>&, ProjectMutationOrigin) override;
        bool removeDeviceInputFromTrack(int32_t, int32_t, ProjectMutationOrigin) override;

        bool connectTrackGraph(
            int32_t, const uapmd_graph::AudioPluginGraphConnection&,
            std::string&, ProjectMutationOrigin) override;
        bool disconnectTrackGraphConnection(
            int32_t, int64_t, std::string&, ProjectMutationOrigin) override;
        bool replaceTrackGraphType(
            int32_t, const std::string&, size_t, ProjectMutationOrigin) override;

        bool setMasterTrackMarkers(std::vector<ClipMarker>, ProjectMutationOrigin) override;
        bool setLatencyCompensationSettings(
            const LatencyCompensationProjectSettings&, ProjectMutationOrigin) override;

    private:
        // Builds the command for one property and runs it through the
        // dispatcher. This is the whole of the per-property boilerplate.
        template<typename Property>
        bool execute(
            typename Property::Address address,
            typename Property::Value value,
            ProjectMutationOrigin origin,
            std::string label = {}) {
            return dispatch_
                .executeSynchronously(
                    std::make_shared<timeline_detail::PropertyCommand<Property>>(
                        target_, std::move(address), std::move(value), std::move(label)),
                    origin)
                .succeeded();
        }

        // Same as execute(), but hands back the reason a command failed for
        // entry points that report one to their caller.
        template<typename Property>
        ProjectCommandResult executeReporting(
            typename Property::Address address,
            typename Property::Value value,
            ProjectMutationOrigin origin) {
            return dispatch_.executeSynchronously(
                std::make_shared<timeline_detail::PropertyCommand<Property>>(
                    target_, std::move(address), std::move(value)),
                origin);
        }

        // Adding, rerouting and removing a device input are one write of the
        // per-input value; only the label says which the user asked for.
        bool executeDeviceInput(
            int32_t trackIndex,
            int32_t sourceNodeId,
            std::optional<std::vector<uint32_t>> channels,
            ProjectMutationOrigin origin,
            std::string label) {
            auto trackReferenceId = target_.addresses().trackReferenceId(trackIndex);
            return trackReferenceId
                && execute<timeline_detail::DeviceInputChannelsProperty>(
                    DeviceInputAddress{std::move(*trackReferenceId), sourceNodeId},
                    std::move(channels),
                    origin,
                    std::move(label));
        }

        // Each family differs only in how a runtime index becomes the stable
        // address the command carries.
        template<typename Property>
        bool executeClip(
            int32_t trackIndex,
            int32_t clipId,
            typename Property::Value value,
            ProjectMutationOrigin origin) {
            auto address = target_.addresses().clipAddress(trackIndex, clipId);
            return address
                && execute<Property>(std::move(*address), std::move(value), origin);
        }

        template<typename Property>
        bool executeTrack(
            int32_t trackIndex,
            typename Property::Value value,
            ProjectMutationOrigin origin) {
            auto address = target_.addresses().trackReferenceId(trackIndex);
            return address
                && execute<Property>(std::move(*address), std::move(value), origin);
        }

        template<typename Property>
        bool executePlugin(
            int32_t instanceId,
            typename Property::Value value,
            ProjectMutationOrigin origin) {
            auto address = target_.addresses().pluginAddress(instanceId);
            return address
                && execute<Property>(std::move(*address), std::move(value), origin);
        }
    };

} // namespace uapmd
