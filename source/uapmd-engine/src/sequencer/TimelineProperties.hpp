#pragma once

// Property commands: one generic command plus the descriptors that give it
// its per-property behaviour.
//
// Private to the timeline facade implementation; not part of the module's
// public surface. Included only by TimelineFacade.cpp.

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <uapmd-data/uapmd-data.hpp>
#include "uapmd-engine/uapmd-engine.hpp"

#include "TimelineHistoryValues.hpp"

namespace uapmd::timeline_detail {

    // The mutation primitives a property command needs, implemented by
    // TimelineFacadeImpl.
    //
    // A command holds a typed reference to this service instead of an
    // ad-hoc mutation lambda. The payload then stays inspectable data, and
    // the knowledge of how to mutate the document lives in one place
    // rather than being restated wherever a command is created.
    class PropertyCommandTarget {
    public:
        virtual ~PropertyCommandTarget() = default;

        virtual ProjectAddressBook& addresses() = 0;
        virtual double timelineSampleRate() const = 0;

        // Emits the document events for a clip that has just changed.
        virtual void onClipMutated(
            TimelineTrack& track,
            int32_t clipId,
            std::string_view changeType) = 0;
        // Emits a track change event without touching the timeline
        // notification, for changes that are not timeline content.
        virtual void onTrackChanged(
            std::string_view trackReferenceId,
            std::string_view changeType) = 0;
        // The common case: emit the event and notify the timeline.
        virtual void onTrackMutated(
            std::string_view trackReferenceId,
            std::string_view changeType) = 0;
        // Re-resolves every clip anchor after one clip has moved, because
        // an anchor may be expressed relative to another clip.
        virtual void resolveClipAnchors() = 0;

        // Plug-in access, narrowed to what property commands need rather
        // than handing out the engine.
        virtual uapmd_plugin_hosting::AudioPluginInstanceAPI* pluginInstance(int32_t instanceId) = 0;
        virtual bool pluginInstanceBusy(int32_t instanceId) = 0;
        virtual uint8_t pluginInstanceGroup(int32_t instanceId) = 0;
        virtual bool setPluginInstanceGroup(int32_t instanceId, uint8_t group) = 0;
        virtual bool applyPluginParameter(
            std::string_view trackReferenceId,
            std::string_view nodeId,
            int32_t parameterIndex,
            double value) = 0;
        virtual bool setTrackFreezePolicy(int32_t trackIndex, bool enabled) = 0;
        virtual bool trackFreezePolicyEnabled(int32_t trackIndex) const = 0;

        // Project-wide markers live with the master track, which the engine
        // owns. Applying them also re-resolves anchors, because a marker may
        // be what a clip is anchored to.
        virtual std::vector<ClipMarker> masterTrackMarkers() const = 0;
        virtual bool applyMasterTrackMarkers(std::vector<ClipMarker> markers) = 0;

        // The complete persisted latency/monitoring configuration, applied as
        // one value. Callers editing a single field read the current settings,
        // change that field, and write the whole snapshot back.
        virtual LatencyCompensationProjectSettings latencyCompensationSettings() const = 0;
        virtual bool applyLatencyCompensationSettings(
            const LatencyCompensationProjectSettings& settings) = 0;

        // One device input read and written as a single optional value:
        // nullopt means the input is not on the track, so adding, rerouting
        // and removing one are all the same write. That is what lets undo of
        // an add be a remove without a second operation shape.
        virtual std::optional<std::vector<uint32_t>> deviceInputChannels(
            std::string_view trackReferenceId,
            int32_t sourceNodeId) = 0;
        virtual bool applyDeviceInputChannels(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const std::optional<std::vector<uint32_t>>& channels) = 0;

        // Whether one graph connection is currently in a track's graph, and
        // adding or removing it. As with a device input, presence is the
        // value, so disconnecting is the undo of connecting.
        virtual bool graphConnectionPresent(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& connection) = 0;
        virtual bool applyGraphConnectionPresence(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& connection,
            bool present) = 0;

        // The connection carrying this graph-minted id, if the track still
        // has it. The id is a runtime handle, so it is resolved to the
        // connection itself before anything is recorded.
        virtual std::optional<uapmd_graph::AudioPluginGraphConnection>
        graphConnectionById(int32_t trackIndex, int64_t connectionId) = 0;

        // Track graph replacement. The canonical provider id for a requested
        // graph type, or nothing when no provider claims it.
        virtual std::optional<std::string> resolveGraphProviderId(
            const std::string& graphTypeId) = 0;
        virtual std::optional<TrackGraphSnapshot> captureTrackGraph(
            std::string_view trackReferenceId) = 0;
        virtual bool applyTrackGraph(
            std::string_view trackReferenceId,
            const TrackGraphSnapshot& snapshot,
            size_t eventBufferSizeInBytes) = 0;

        // A clip read and written as a whole, as an optional fragment:
        // nullopt means the clip is not on the track. Adding, replacing the
        // content of, and deleting a clip are then one write, and undoing a
        // delete is a restore of the very fragment that was captured.
        virtual std::optional<ProjectClipFragment> clipFragment(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId) = 0;
        virtual bool applyClipFragment(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId,
            const std::optional<ProjectClipFragment>& fragment) = 0;

        // Track presence. Restoring is asynchronous -- it reconstructs the
        // plug-ins the fragment describes -- so it reports through a callback
        // rather than a return value.
        virtual bool removeTrackByReferenceId(std::string_view trackReferenceId) = 0;
        virtual void restoreTrackFragment(
            const ProjectTrackFragment& fragment,
            int32_t insertionIndex,
            std::function<void(bool restored, std::string error)> completion) = 0;

        // Plug-in mutations, all asynchronous because they cross into the
        // hosted plug-in: writing opaque state, loading a numbered preset, and
        // creating or destroying the instance itself.
        virtual void writePluginState(
            const PluginAddress& address,
            const std::vector<uint8_t>& state,
            ProjectCommandCompletion completion) = 0;
        virtual void writePluginPreset(
            const PluginAddress& address,
            int32_t presetIndex,
            ProjectCommandCompletion completion) = 0;
        virtual void writePluginPresence(
            const PluginAddress& address,
            const std::optional<PluginInstanceSnapshot>& snapshot,
            ProjectCommandCompletion completion) = 0;

        // Why the most recent write() returned false, when it can fail for
        // more than one reason. Writes are serialized on the model thread, so
        // the value read straight after a failed write is that write's own.
        // Empty unless the failing property reports detail.
        virtual std::string lastWriteFailure() const = 0;
    };

    // One property edit, whatever the property is attached to.
    //
    // `Property` supplies the static behaviour -- how to find the subject,
    // which field, how to read and write it, what the change is called --
    // so an instance is nothing but an address and a value. Undo is the
    // same command type carrying the value that was there before, which is
    // why this single class covers clip, track and plug-in properties
    // alike instead of one class per family, let alone per property.
    template<typename Property>
    class PropertyCommand final : public ProjectCommand {
        using Value = typename Property::Value;
        using Address = typename Property::Address;

        PropertyCommandTarget& target_;
        Address address_;
        Value value_;
        // Set only where the value alone cannot say what the user did. Adding
        // a device input and changing its routing write the same kind of
        // value, and only the caller knows which one it asked for; leaving the
        // history to guess would label both the same.
        std::string label_{};

    public:
        PropertyCommand(
            PropertyCommandTarget& target,
            Address address,
            Value value,
            std::string label = {})
            : target_(target)
            , address_(std::move(address))
            , value_(std::move(value))
            , label_(std::move(label)) {
        }

        std::string_view commandId() const override {
            return Property::commandId;
        }

        bool batchesDocumentEvents() const override {
            return Property::batchesDocumentEvents;
        }

        std::string description() const override {
            return label_.empty() ? Property::describe(value_) : label_;
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + retainedValueSize(address_)
                + retainedValueSize(value_)
                + label_.capacity();
        }

        bool mergeWith(const ProjectCommand& subsequent) override {
            // The manager only merges commands sharing a commandId(), so
            // the type is already known.
            const auto& next = static_cast<const PropertyCommand&>(subsequent);
            if (!(next.address_ == address_))
                return false;
            value_ = next.value_;
            return true;
        }

        void execute(
            ProjectCommandContext& context,
            ProjectCommandCompletion completion) override {
            auto subject = Property::resolve(target_, address_);
            if (!subject) {
                completion(ProjectCommandResult::failure(std::format(
                    "The subject of '{}' no longer exists.",
                    Property::commandId)));
                return;
            }

            auto before = Property::read(target_, *subject);
            if (Property::equal(before, value_)) {
                // No revert recorded, so no history entry is created.
                completion(ProjectCommandResult::success());
                return;
            }

            if (!Property::write(target_, *subject, value_)) {
                auto reason = Property::failureMessage(target_);
                completion(ProjectCommandResult::failure(
                    reason.empty()
                        ? std::format("Could not apply '{}'.", Property::commandId)
                        : std::move(reason)));
                return;
            }
            Property::notify(target_, *subject);
            context.recordRevert(std::make_shared<PropertyCommand>(
                target_, address_, std::move(before), label_));
            completion(ProjectCommandResult::success());
        }
    };

    // Defaults every property descriptor inherits. A descriptor states
    // only what is peculiar to it: most compare with ==, and most have a
    // fixed history label.
    template<typename Derived, typename ValueType>
    struct PropertyDescriptor {
        using Value = ValueType;

        static bool equal(const Value& lhs, const Value& rhs) {
            return lhs == rhs;
        }

        static std::string describe(const Value&) {
            return std::string(Derived::label);
        }

        // Most property edits are a single inline mutation, so batching their
        // events costs nothing and spares observers a half-applied read. A
        // descriptor whose read or write must not run inside a document
        // transaction -- anything that captures a fragment, because extension
        // state such as an ARA archive cannot be taken mid-edit -- sets this
        // to false.
        static constexpr bool batchesDocumentEvents = true;

        // Most properties can only fail one way, so the generic message says
        // everything there is to say. A property whose write is rejected for
        // several distinct reasons overrides this so the reason survives into
        // the history result, where a failed undo reports it to the user.
        static std::string failureMessage(PropertyCommandTarget&) {
            return {};
        }
    };

    // A clip is addressed by (track, clip) and mutated through its
    // ClipManager.
    struct ClipSubject {
        TimelineTrack* track{};
        int32_t clipId{-1};
        const ClipData* clip{};
    };

    template<typename Derived, typename ValueType>
    struct ClipPropertyDescriptor : PropertyDescriptor<Derived, ValueType> {
        using Address = ClipAddress;
        using Subject = ClipSubject;

        static std::optional<ClipSubject> resolve(
            PropertyCommandTarget& target,
            const ClipAddress& address) {
            auto& addresses = target.addresses();
            auto* track = addresses.timelineTrack(address.trackReferenceId);
            const auto clipId = addresses.clipId(address);
            const auto* clip = track && clipId >= 0
                ? track->clipManager().getClip(clipId)
                : nullptr;
            if (!clip)
                return std::nullopt;
            return ClipSubject{.track = track, .clipId = clipId, .clip = clip};
        }

        static void notify(PropertyCommandTarget& target, const ClipSubject& subject) {
            target.onClipMutated(*subject.track, subject.clipId, Derived::changeType);
        }
    };

    // A track is addressed by its reference id and mutated through its
    // SequencerTrack.
    struct TrackSubject {
        SequencerTrack* track{};
        int32_t index{-1};
        ProjectObjectId referenceId{};
    };

    template<typename Derived, typename ValueType>
    struct TrackPropertyDescriptor : PropertyDescriptor<Derived, ValueType> {
        using Address = ProjectObjectId;
        using Subject = TrackSubject;

        static std::optional<TrackSubject> resolve(
            PropertyCommandTarget& target,
            const ProjectObjectId& address) {
            auto& addresses = target.addresses();
            auto* track = addresses.sequencerTrack(address);
            if (!track)
                return std::nullopt;
            return TrackSubject{
                .track = track,
                .index = addresses.trackIndex(address),
                .referenceId = address
            };
        }

        static void notify(PropertyCommandTarget& target, const TrackSubject& subject) {
            target.onTrackMutated(subject.referenceId, Derived::changeType);
        }
    };

    // A plug-in is addressed by (track, node); its runtime instance id is
    // resolved afresh on every run because restoring a plug-in gives it a
    // new one.
    struct PluginSubject {
        int32_t instanceId{-1};
        uapmd_plugin_hosting::AudioPluginInstanceAPI* instance{};
        PluginAddress address{};
    };

    template<typename Derived, typename ValueType>
    struct PluginPropertyDescriptor : PropertyDescriptor<Derived, ValueType> {
        using Address = PluginAddress;
        using Subject = PluginSubject;

        static std::optional<PluginSubject> resolve(
            PropertyCommandTarget& target,
            const PluginAddress& address) {
            const auto instanceId = target.addresses().pluginInstanceId(address);
            auto* instance = target.pluginInstance(instanceId);
            if (!instance)
                return std::nullopt;
            return PluginSubject{
                .instanceId = instanceId,
                .instance = instance,
                .address = address
            };
        }

        static void notify(PropertyCommandTarget& target, const PluginSubject& subject) {
            target.onTrackMutated(subject.address.trackReferenceId, Derived::changeType);
        }
    };

    struct ClipEnabledProperty : ClipPropertyDescriptor<ClipEnabledProperty, bool> {
        static constexpr std::string_view commandId{"clip.setEnabled"};
        static constexpr std::string_view changeType{"clip-enablement-changed"};

        static std::string describe(bool value) {
            return value ? "Enable clip" : "Disable clip";
        }

        static bool read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->enabled;
        }

        static bool write(PropertyCommandTarget&, const ClipSubject& subject, bool value) {
            return subject.track->clipManager().setClipEnabled(subject.clipId, value);
        }
    };

    struct ClipAnchorProperty : ClipPropertyDescriptor<ClipAnchorProperty, TimeReference> {
        static constexpr std::string_view commandId{"clip.setAnchor"};
        static constexpr std::string_view changeType{"clip-position-changed"};
        static constexpr std::string_view label{"Move clip"};

        static TimeReference read(PropertyCommandTarget& target, const ClipSubject& subject) {
            return subject.clip->timeReference(target.timelineSampleRate());
        }

        static bool write(
            PropertyCommandTarget& target,
            const ClipSubject& subject,
            const TimeReference& value) {
            if (!subject.track->clipManager().setClipAnchor(
                    subject.clipId, value, target.timelineSampleRate()))
                return false;
            target.resolveClipAnchors();
            return true;
        }
    };

    struct ClipGainProperty : ClipPropertyDescriptor<ClipGainProperty, double> {
        static constexpr std::string_view commandId{"clip.setGain"};
        static constexpr std::string_view changeType{"clip-gain-changed"};
        static constexpr std::string_view label{"Change clip gain"};

        static double read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->gain;
        }

        static bool write(PropertyCommandTarget&, const ClipSubject& subject, double value) {
            return subject.track->clipManager().setClipGain(subject.clipId, value);
        }
    };

    struct ClipMutedProperty : ClipPropertyDescriptor<ClipMutedProperty, bool> {
        static constexpr std::string_view commandId{"clip.setMuted"};
        static constexpr std::string_view changeType{"clip-mute-changed"};

        static std::string describe(bool value) {
            return value ? "Mute clip" : "Unmute clip";
        }

        static bool read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->muted;
        }

        static bool write(PropertyCommandTarget&, const ClipSubject& subject, bool value) {
            return subject.track->clipManager().setClipMuted(subject.clipId, value);
        }
    };

    struct ClipDurationProperty : ClipPropertyDescriptor<ClipDurationProperty, int64_t> {
        static constexpr std::string_view commandId{"clip.resize"};
        static constexpr std::string_view changeType{"clip-duration-changed"};
        static constexpr std::string_view label{"Resize clip"};

        static int64_t read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->durationSamples;
        }

        static bool write(PropertyCommandTarget&, const ClipSubject& subject, int64_t value) {
            return subject.track->clipManager().resizeClip(subject.clipId, value);
        }
    };

    struct ClipNameProperty : ClipPropertyDescriptor<ClipNameProperty, std::string> {
        static constexpr std::string_view commandId{"clip.setName"};
        static constexpr std::string_view changeType{"clip-name-changed"};
        static constexpr std::string_view label{"Rename clip"};

        static std::string read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->name;
        }

        static bool write(
            PropertyCommandTarget&,
            const ClipSubject& subject,
            const std::string& value) {
            return subject.track->clipManager().setClipName(subject.clipId, value);
        }
    };

    struct ClipFilepathProperty : ClipPropertyDescriptor<ClipFilepathProperty, std::string> {
        static constexpr std::string_view commandId{"clip.setFilepath"};
        static constexpr std::string_view changeType{"clip-content-changed"};
        static constexpr std::string_view label{"Change clip file"};

        static std::string read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->filepath;
        }

        static bool write(
            PropertyCommandTarget&,
            const ClipSubject& subject,
            const std::string& value) {
            return subject.track->clipManager().setClipFilepath(subject.clipId, value);
        }
    };

    struct ClipNeedsFileSaveProperty
        : ClipPropertyDescriptor<ClipNeedsFileSaveProperty, bool> {
        static constexpr std::string_view commandId{"clip.setNeedsFileSave"};
        static constexpr std::string_view changeType{"clip-content-changed"};
        static constexpr std::string_view label{"Change clip save state"};

        static bool read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->needsFileSave;
        }

        static bool write(PropertyCommandTarget&, const ClipSubject& subject, bool value) {
            return subject.track->clipManager().setClipNeedsFileSave(subject.clipId, value);
        }
    };

    struct ClipMarkersProperty
        : ClipPropertyDescriptor<ClipMarkersProperty, std::vector<ClipMarker>> {
        static constexpr std::string_view commandId{"clip.setMarkers"};
        static constexpr std::string_view changeType{"clip-content-changed"};
        static constexpr std::string_view label{"Edit clip markers"};

        static Value read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->markers;
        }

        static bool equal(const Value& lhs, const Value& rhs) {
            return clipMarkersEqual(lhs, rhs);
        }

        static bool write(
            PropertyCommandTarget&,
            const ClipSubject& subject,
            const Value& value) {
            return subject.track->clipManager().setClipMarkers(subject.clipId, value);
        }
    };

    struct ClipAudioWarpsProperty
        : ClipPropertyDescriptor<ClipAudioWarpsProperty, std::vector<AudioWarpPoint>> {
        static constexpr std::string_view commandId{"clip.setAudioWarps"};
        static constexpr std::string_view changeType{"clip-content-changed"};
        static constexpr std::string_view label{"Edit clip warps"};

        static Value read(PropertyCommandTarget&, const ClipSubject& subject) {
            return subject.clip->audioWarps;
        }

        static bool equal(const Value& lhs, const Value& rhs) {
            return audioWarpPointsEqual(lhs, rhs);
        }

        static bool write(
            PropertyCommandTarget&,
            const ClipSubject& subject,
            const Value& value) {
            return subject.track->clipManager().setAudioWarps(subject.clipId, value);
        }
    };

    struct TrackGainProperty : TrackPropertyDescriptor<TrackGainProperty, double> {
        static constexpr std::string_view commandId{"track.setGain"};
        static constexpr std::string_view changeType{"track-gain-changed"};
        static constexpr std::string_view label{"Change track gain"};

        static double read(PropertyCommandTarget&, const TrackSubject& subject) {
            return subject.track->trackGain();
        }

        static bool write(PropertyCommandTarget&, const TrackSubject& subject, double value) {
            return subject.track->trackGain(value);
        }
    };

    struct TrackMutedProperty : TrackPropertyDescriptor<TrackMutedProperty, bool> {
        static constexpr std::string_view commandId{"track.setMuted"};
        static constexpr std::string_view changeType{"track-mute-changed"};

        static std::string describe(bool value) {
            return value ? "Mute track" : "Unmute track";
        }

        static bool read(PropertyCommandTarget&, const TrackSubject& subject) {
            return subject.track->muted();
        }

        static bool write(PropertyCommandTarget&, const TrackSubject& subject, bool value) {
            subject.track->muted(value);
            return true;
        }
    };

    struct TrackSoloProperty : TrackPropertyDescriptor<TrackSoloProperty, bool> {
        static constexpr std::string_view commandId{"track.setSolo"};
        static constexpr std::string_view changeType{"track-solo-changed"};

        static std::string describe(bool value) {
            return value ? "Solo track" : "Unsolo track";
        }

        static bool read(PropertyCommandTarget&, const TrackSubject& subject) {
            return subject.track->solo();
        }

        static bool write(PropertyCommandTarget&, const TrackSubject& subject, bool value) {
            subject.track->solo(value);
            return true;
        }
    };

    struct TrackBypassedProperty : TrackPropertyDescriptor<TrackBypassedProperty, bool> {
        static constexpr std::string_view commandId{"track.setBypassed"};
        static constexpr std::string_view changeType{"track-bypass-changed"};

        static std::string describe(bool value) {
            return value ? "Bypass track" : "Enable track processing";
        }

        static bool read(PropertyCommandTarget&, const TrackSubject& subject) {
            return subject.track->bypassed();
        }

        static bool write(PropertyCommandTarget&, const TrackSubject& subject, bool value) {
            subject.track->bypassed(value);
            return true;
        }
    };

    struct TrackFreezePolicyProperty
        : TrackPropertyDescriptor<TrackFreezePolicyProperty, bool> {
        static constexpr std::string_view commandId{"track.setFreezePolicy"};
        static constexpr std::string_view changeType{"track-freeze-policy-changed"};

        static std::string describe(bool value) {
            return value ? "Freeze track" : "Unfreeze track";
        }

        static bool read(PropertyCommandTarget& target, const TrackSubject& subject) {
            return target.trackFreezePolicyEnabled(subject.index);
        }

        static bool write(PropertyCommandTarget& target, const TrackSubject& subject, bool value) {
            return target.setTrackFreezePolicy(subject.index, value);
        }

        // Freeze policy is not timeline content, so it emits the track
        // event without a timeline notification.
        static void notify(PropertyCommandTarget& target, const TrackSubject& subject) {
            target.onTrackChanged(subject.referenceId, changeType);
        }
    };

    struct PluginBypassedProperty : PluginPropertyDescriptor<PluginBypassedProperty, bool> {
        static constexpr std::string_view commandId{"plugin.setBypassed"};
        static constexpr std::string_view changeType{"plugin-bypass-changed"};

        static std::string describe(bool value) {
            return value ? "Bypass plug-in" : "Enable plug-in";
        }

        static bool read(PropertyCommandTarget&, const PluginSubject& subject) {
            return subject.instance->bypassed();
        }

        static bool write(PropertyCommandTarget&, const PluginSubject& subject, bool value) {
            subject.instance->bypassed(value);
            return true;
        }
    };

    struct PluginGroupProperty : PluginPropertyDescriptor<PluginGroupProperty, uint8_t> {
        static constexpr std::string_view commandId{"plugin.setGroup"};
        static constexpr std::string_view changeType{"plugin-group-changed"};
        static constexpr std::string_view label{"Change plug-in UMP group"};

        static uint8_t read(PropertyCommandTarget& target, const PluginSubject& subject) {
            return target.pluginInstanceGroup(subject.instanceId);
        }

        static bool write(
            PropertyCommandTarget& target,
            const PluginSubject& subject,
            uint8_t value) {
            return target.setPluginInstanceGroup(subject.instanceId, value);
        }
    };

    // A parameter's identity includes its index, so the index belongs in
    // the address rather than in a per-call formatted property key.
    struct PluginParameterAddress {
        PluginAddress plugin{};
        int32_t parameterIndex{-1};

        bool operator==(const PluginParameterAddress&) const = default;
    };

    inline size_t retainedValueSize(const PluginParameterAddress& value) {
        return sizeof(value) + retainedValueSize(value.plugin);
    }

    struct PluginParameterSubject {
        PluginSubject plugin{};
        PluginParameterAddress address{};
    };

    struct PluginParameterProperty
        : PropertyDescriptor<PluginParameterProperty, double> {
        using Address = PluginParameterAddress;
        using Subject = PluginParameterSubject;

        static constexpr std::string_view commandId{"plugin.setParameter"};
        static constexpr std::string_view label{"Change plug-in parameter"};

        static std::optional<PluginParameterSubject> resolve(
            PropertyCommandTarget& target,
            const PluginParameterAddress& address) {
            const auto instanceId = target.addresses().pluginInstanceId(address.plugin);
            auto* instance = target.pluginInstance(instanceId);
            if (!instance)
                return std::nullopt;
            return PluginParameterSubject{
                .plugin = {
                    .instanceId = instanceId,
                    .instance = instance,
                    .address = address.plugin
                },
                .address = address
            };
        }

        static double read(PropertyCommandTarget&, const PluginParameterSubject& subject) {
            return subject.plugin.instance->getParameterValue(subject.address.parameterIndex);
        }

        static bool write(
            PropertyCommandTarget& target,
            const PluginParameterSubject& subject,
            double value) {
            return target.applyPluginParameter(
                subject.address.plugin.trackReferenceId,
                subject.address.plugin.nodeId,
                subject.address.parameterIndex,
                value);
        }

        // applyPluginParameter already emits the per-index change event
        // and notifies the timeline. The previous code emitted the same
        // event a second time around it.
        static void notify(PropertyCommandTarget&, const PluginParameterSubject&) {
        }
    };

    struct PluginPerNoteAddress {
        PluginAddress plugin{};
        remidy::PerNoteControllerContextTypes contextType{};
        remidy::PerNoteControllerContext context{};
        int32_t parameterIndex{-1};

        bool operator==(const PluginPerNoteAddress& other) const {
            return plugin == other.plugin
                && contextType == other.contextType
                && context.note == other.context.note
                && context.channel == other.context.channel
                && context.group == other.context.group
                && parameterIndex == other.parameterIndex;
        }
    };

    inline size_t retainedValueSize(const PluginPerNoteAddress& value) {
        return sizeof(value) + retainedValueSize(value.plugin);
    }

    struct PluginPerNoteSubject {
        PluginSubject plugin{};
        PluginPerNoteAddress address{};
        double current{};
    };

    // Per-note controller edits share the plug-in's persistent identity
    // and history, so changing the selected key, channel or group does not
    // silently bypass undo.
    struct PluginPerNoteProperty
        : PropertyDescriptor<PluginPerNoteProperty, double> {
        using Address = PluginPerNoteAddress;
        using Subject = PluginPerNoteSubject;

        static constexpr std::string_view commandId{"plugin.setPerNoteController"};
        static constexpr std::string_view changeType{"plugin-per-note-parameter-changed"};
        static constexpr std::string_view label{"Change per-note plug-in parameter"};

        static std::optional<PluginPerNoteSubject> resolve(
            PropertyCommandTarget& target,
            const PluginPerNoteAddress& address) {
            const auto instanceId = target.addresses().pluginInstanceId(address.plugin);
            auto* instance = target.pluginInstance(instanceId);
            if (!instance || address.parameterIndex < 0)
                return std::nullopt;

            double current = std::numeric_limits<double>::quiet_NaN();
            auto* parameterSupport = instance->parameterSupport();
            const bool readable = (parameterSupport
                    && parameterSupport->getPerNoteController(
                           address.context,
                           static_cast<uint32_t>(address.parameterIndex),
                           &current)
                        == remidy::StatusCode::OK)
                // Older adapters expose note-scoped values only through
                // the AudioPluginInstanceAPI convenience method.
                || (address.contextType
                        == remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE
                    && address.context.note <= UINT8_MAX
                    && address.parameterIndex <= UINT8_MAX
                    && instance->getPerNoteControllerValue(
                        static_cast<uint8_t>(address.context.note),
                        static_cast<uint8_t>(address.parameterIndex),
                        &current));
            if (!readable)
                return std::nullopt;
            return PluginPerNoteSubject{
                .plugin = {
                    .instanceId = instanceId,
                    .instance = instance,
                    .address = address.plugin
                },
                .address = address,
                .current = current
            };
        }

        static double read(PropertyCommandTarget&, const PluginPerNoteSubject& subject) {
            return subject.current;
        }

        // A value the plug-in could not report is treated as "no change"
        // rather than as a failure, matching the previous behaviour.
        static bool equal(double lhs, double rhs) {
            return !std::isfinite(lhs) || lhs == rhs;
        }

        static bool write(
            PropertyCommandTarget& target,
            const PluginPerNoteSubject& subject,
            double value) {
            auto* instance = subject.plugin.instance;
            if (target.pluginInstanceBusy(subject.plugin.instanceId))
                return false;
            const auto& address = subject.address;
            auto* support = instance->parameterSupport();
            if (support
                && support->setPerNoteController(
                       address.context,
                       static_cast<uint32_t>(address.parameterIndex),
                       value)
                    == remidy::StatusCode::OK)
                return true;
            if (address.contextType
                    != remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE)
                return false;
            if (address.context.note > UINT8_MAX || address.parameterIndex > UINT8_MAX)
                return false;
            instance->setPerNoteControllerValue(
                static_cast<uint8_t>(address.context.note),
                static_cast<uint8_t>(address.parameterIndex),
                value);
            return true;
        }

        static void notify(PropertyCommandTarget& target, const PluginPerNoteSubject& subject) {
            target.onTrackMutated(subject.address.plugin.trackReferenceId, changeType);
        }
    };

    // Replacing a track's graph type, as a command in its own right rather
    // than a property.
    //
    // A property is an address and a value; this also needs the event buffer
    // size to rebuild with, which is neither. Callers supply it from their own
    // configuration -- the application model, the engine, the project loader --
    // and although those agree today, nothing makes them agree, so the value
    // the caller asked for travels with the command.
    //
    // Applying a snapshot replaces the graph wholesale rather than migrating
    // the existing one, so the forward payload is just the requested type and
    // the revert carries the complete previous graph, serialized bytes and all.
    class TrackGraphTypeCommand final : public ProjectCommand {
        PropertyCommandTarget& target_;
        ProjectObjectId track_reference_id_;
        TrackGraphSnapshot snapshot_;
        size_t event_buffer_size_in_bytes_;

    public:
        TrackGraphTypeCommand(
            PropertyCommandTarget& target,
            ProjectObjectId trackReferenceId,
            TrackGraphSnapshot snapshot,
            size_t eventBufferSizeInBytes)
            : target_(target)
            , track_reference_id_(std::move(trackReferenceId))
            , snapshot_(std::move(snapshot))
            , event_buffer_size_in_bytes_(eventBufferSizeInBytes) {
        }

        std::string_view commandId() const override {
            return "track.replaceGraphType";
        }

        std::string description() const override {
            return "Change track graph type";
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + track_reference_id_.capacity()
                + retainedValueSize(snapshot_);
        }

        void execute(
            ProjectCommandContext& context,
            ProjectCommandCompletion completion) override {
            auto providerId = target_.resolveGraphProviderId(snapshot_.graphType);
            if (!providerId) {
                completion(ProjectCommandResult::failure(std::format(
                    "No graph provider for '{}'.", snapshot_.graphType)));
                return;
            }

            auto before = target_.captureTrackGraph(track_reference_id_);
            if (!before) {
                completion(ProjectCommandResult::failure(
                    "Could not capture the current track graph."));
                return;
            }
            // Already this type, with no graph content to restore: nothing to
            // do, and no revert recorded, so no history entry appears.
            if (before->graphType == *providerId && snapshot_.graphBytes.empty()) {
                completion(ProjectCommandResult::success());
                return;
            }

            if (!target_.applyTrackGraph(
                    track_reference_id_, snapshot_, event_buffer_size_in_bytes_)) {
                // The old graph is already gone by the time deserialization
                // can fail, so put it back rather than leaving the track with
                // whichever half-built graph the failure produced.
                target_.applyTrackGraph(
                    track_reference_id_, *before, event_buffer_size_in_bytes_);
                completion(ProjectCommandResult::failure(
                    "Could not replace the track graph."));
                return;
            }

            context.recordRevert(std::make_shared<TrackGraphTypeCommand>(
                target_,
                track_reference_id_,
                std::move(*before),
                event_buffer_size_in_bytes_));
            completion(ProjectCommandResult::success());
        }
    };

    // Presence of one track, as an asynchronous command.
    //
    // Same idea as clip presence -- nothing means the track is not there, and
    // undoing an addition is a removal -- but restoring a track rebuilds its
    // plug-in graph, so this cannot be a PropertyCommand: those read and write
    // inline.
    //
    // Unlike a property command this does not derive its own inverse. Deriving
    // it would mean capturing the track, which is itself asynchronous and
    // which the caller has already done before getting here; both directions
    // are therefore supplied as a pair through
    // ProjectCommandManager::recordExecuted(), and this command only ever
    // applies its own value.
    class TrackPresenceCommand final : public ProjectCommand {
        PropertyCommandTarget& target_;
        ProjectObjectId track_reference_id_;
        int32_t insertion_index_;
        std::optional<ProjectTrackFragment> fragment_;
        std::string label_;

    public:
        TrackPresenceCommand(
            PropertyCommandTarget& target,
            ProjectObjectId trackReferenceId,
            int32_t insertionIndex,
            std::optional<ProjectTrackFragment> fragment,
            std::string label)
            : target_(target)
            , track_reference_id_(std::move(trackReferenceId))
            , insertion_index_(insertionIndex)
            , fragment_(std::move(fragment))
            , label_(std::move(label)) {
        }

        std::string_view commandId() const override {
            return "track.setPresence";
        }

        std::string description() const override {
            return label_;
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + track_reference_id_.capacity()
                + label_.capacity()
                + (fragment_ ? retainedValueSize(*fragment_) : 0);
        }

        // Restoring constructs plug-ins and reads their state; a document
        // transaction must not stay open across that.
        bool batchesDocumentEvents() const override {
            return false;
        }

        // The history engine marshals completions back to the model thread
        // itself, so this reports from wherever the restore finished.
        void execute(
            ProjectCommandContext&,
            ProjectCommandCompletion completion) override {
            if (!fragment_) {
                completion(
                    target_.removeTrackByReferenceId(track_reference_id_)
                        ? ProjectCommandResult::success()
                        : ProjectCommandResult::failure(std::format(
                            "Could not remove track {}.", track_reference_id_)));
                return;
            }
            target_.restoreTrackFragment(
                *fragment_,
                insertion_index_,
                [referenceId = track_reference_id_,
                 completion = std::move(completion)](
                    bool restored, std::string error) mutable {
                    if (restored) {
                        completion(ProjectCommandResult::success());
                        return;
                    }
                    completion(ProjectCommandResult::failure(
                        error.empty()
                            ? std::format("Could not restore track {}.", referenceId)
                            : std::move(error)));
                });
        }
    };

    // The plug-in commands below all reach a hosted plug-in and therefore
    // finish asynchronously, and all of them are recorded as explicit pairs:
    // capturing the state a command would need to derive its own inverse is
    // itself an asynchronous request, which the caller has already made.
    class PluginCommandBase : public ProjectCommand {
    protected:
        PropertyCommandTarget& target_;
        PluginAddress address_;
        std::string label_;

        PluginCommandBase(
            PropertyCommandTarget& target,
            PluginAddress address,
            std::string label)
            : target_(target)
            , address_(std::move(address))
            , label_(std::move(label)) {
        }

    public:
        std::string description() const override {
            return label_;
        }

        // Plug-in work cannot run inside a document transaction: state
        // requests and instantiation both cross into the plug-in, and an ARA
        // document cannot be archived while an edit is open.
        bool batchesDocumentEvents() const override {
            return false;
        }
    };

    // Writes one opaque state blob into the addressed plug-in.
    class PluginStateCommand final : public PluginCommandBase {
        std::vector<uint8_t> state_;

    public:
        PluginStateCommand(
            PropertyCommandTarget& target,
            PluginAddress address,
            std::vector<uint8_t> state,
            std::string label)
            : PluginCommandBase(target, std::move(address), std::move(label))
            , state_(std::move(state)) {
        }

        std::string_view commandId() const override {
            return "plugin.setState";
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + address_.trackReferenceId.capacity()
                + address_.nodeId.capacity()
                + label_.capacity()
                + state_.capacity();
        }

        void execute(
            ProjectCommandContext&,
            ProjectCommandCompletion completion) override {
            target_.writePluginState(address_, state_, std::move(completion));
        }
    };

    // Loads a numbered preset.
    //
    // Its inverse is a PluginStateCommand holding the bytes the preset
    // replaced, so the two directions of this history entry are different
    // command types. That is what redoing a preset load should mean: load the
    // preset again, rather than write back whatever bytes it happened to
    // produce the first time.
    class PluginPresetCommand final : public PluginCommandBase {
        int32_t preset_index_;

    public:
        PluginPresetCommand(
            PropertyCommandTarget& target,
            PluginAddress address,
            int32_t presetIndex,
            std::string label)
            : PluginCommandBase(target, std::move(address), std::move(label))
            , preset_index_(presetIndex) {
        }

        std::string_view commandId() const override {
            return "plugin.loadPreset";
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + address_.trackReferenceId.capacity()
                + address_.nodeId.capacity()
                + label_.capacity();
        }

        void execute(
            ProjectCommandContext&,
            ProjectCommandCompletion completion) override {
            target_.writePluginPreset(address_, preset_index_, std::move(completion));
        }
    };

    // Presence of one plug-in instance: nothing means it is not in the graph,
    // so undoing an insertion is a removal and undoing a removal recreates the
    // plug-in from the snapshot under its original node identity.
    class PluginPresenceCommand final : public PluginCommandBase {
        std::optional<PluginInstanceSnapshot> snapshot_;

    public:
        PluginPresenceCommand(
            PropertyCommandTarget& target,
            PluginAddress address,
            std::optional<PluginInstanceSnapshot> snapshot,
            std::string label)
            : PluginCommandBase(target, std::move(address), std::move(label))
            , snapshot_(std::move(snapshot)) {
        }

        std::string_view commandId() const override {
            return "plugin.setPresence";
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + address_.trackReferenceId.capacity()
                + address_.nodeId.capacity()
                + label_.capacity()
                + (snapshot_ ? retainedValueSize(*snapshot_) : 0);
        }

        void execute(
            ProjectCommandContext&,
            ProjectCommandCompletion completion) override {
            target_.writePluginPresence(address_, snapshot_, std::move(completion));
        }
    };

} // namespace uapmd::timeline_detail

namespace uapmd::timeline_detail {

    // Some properties belong to the project as a whole rather than to an
    // addressed object, so their address is empty and always resolves.
    struct ProjectSubject {};

    template<typename Derived, typename ValueType>
    struct ProjectPropertyDescriptor : PropertyDescriptor<Derived, ValueType> {
        using Address = std::monostate;
        using Subject = ProjectSubject;

        static std::optional<ProjectSubject> resolve(
            PropertyCommandTarget&,
            const std::monostate&) {
            return ProjectSubject{};
        }

        static void notify(PropertyCommandTarget&, const ProjectSubject&) {
        }
    };

    struct MasterTrackMarkersProperty
        : ProjectPropertyDescriptor<MasterTrackMarkersProperty, std::vector<ClipMarker>> {
        static constexpr std::string_view commandId{"project.setMasterTrackMarkers"};
        static constexpr std::string_view label{"Edit master markers"};

        static Value read(PropertyCommandTarget& target, const ProjectSubject&) {
            return target.masterTrackMarkers();
        }

        static bool equal(const Value& lhs, const Value& rhs) {
            return clipMarkersEqual(lhs, rhs);
        }

        static bool write(
            PropertyCommandTarget& target,
            const ProjectSubject&,
            const Value& value) {
            return target.applyMasterTrackMarkers(value);
        }
    };

    // A device input is addressed by (track, source node) and carries the
    // channel list it routes, or nothing when it is absent.
    struct DeviceInputSubject {
        DeviceInputAddress address;
    };

    struct DeviceInputChannelsProperty
        : PropertyDescriptor<
              DeviceInputChannelsProperty,
              std::optional<std::vector<uint32_t>>> {
        using Address = DeviceInputAddress;
        using Subject = DeviceInputSubject;

        static constexpr std::string_view commandId{"track.setDeviceInputChannels"};
        static constexpr std::string_view label{"Change device input routing"};

        // The track need not currently hold this input -- writing a value is
        // what creates it -- so the address always resolves and write()
        // reports a track that has genuinely gone away.
        static std::optional<DeviceInputSubject> resolve(
            PropertyCommandTarget&,
            const DeviceInputAddress& address) {
            return DeviceInputSubject{address};
        }

        static Value read(PropertyCommandTarget& target, const DeviceInputSubject& subject) {
            return target.deviceInputChannels(
                subject.address.trackReferenceId, subject.address.sourceNodeId);
        }

        static bool write(
            PropertyCommandTarget& target,
            const DeviceInputSubject& subject,
            const Value& value) {
            return target.applyDeviceInputChannels(
                subject.address.trackReferenceId, subject.address.sourceNodeId, value);
        }

        static void notify(PropertyCommandTarget&, const DeviceInputSubject&) {
        }
    };

    // Presence and content of one clip. The address always resolves: an
    // absent clip is a legitimate state of it, not a missing subject.
    struct ClipPresenceSubject {
        ClipAddress address;
    };

    struct ClipPresenceProperty
        : PropertyDescriptor<ClipPresenceProperty, std::optional<ProjectClipFragment>> {
        using Address = ClipAddress;
        using Subject = ClipPresenceSubject;

        static constexpr std::string_view commandId{"clip.setPresence"};
        static constexpr std::string_view label{"Change clip"};

        // Reading this property captures a clip fragment, which collects
        // extension-owned state and therefore cannot run inside a document
        // transaction. Batching here would silently read an absent clip and
        // record "delete it" as the way to undo a content change.
        static constexpr bool batchesDocumentEvents = false;

        static std::string describe(const Value& value) {
            return value ? "Add clip" : "Delete clip";
        }

        // Two absences are the same state; anything else is recorded. A
        // fragment is a deep capture of clip content, and comparing two of
        // them would cost more than the redundant history entry it saves.
        static bool equal(const Value& lhs, const Value& rhs) {
            return !lhs && !rhs;
        }

        static std::optional<ClipPresenceSubject> resolve(
            PropertyCommandTarget&,
            const ClipAddress& address) {
            return ClipPresenceSubject{address};
        }

        static Value read(PropertyCommandTarget& target, const ClipPresenceSubject& subject) {
            return target.clipFragment(
                subject.address.trackReferenceId, subject.address.clipReferenceId);
        }

        static bool write(
            PropertyCommandTarget& target,
            const ClipPresenceSubject& subject,
            const Value& value) {
            return target.applyClipFragment(
                subject.address.trackReferenceId, subject.address.clipReferenceId, value);
        }

        static void notify(PropertyCommandTarget&, const ClipPresenceSubject&) {
        }
    };

    struct GraphConnectionSubject {
        GraphConnectionAddress address;
    };

    struct GraphConnectionPresentProperty
        : PropertyDescriptor<GraphConnectionPresentProperty, bool> {
        using Address = GraphConnectionAddress;
        using Subject = GraphConnectionSubject;

        static constexpr std::string_view commandId{"track.setGraphConnectionPresent"};
        static constexpr std::string_view label{"Connect track graph"};

        static std::string describe(bool value) {
            return value ? "Connect track graph" : "Disconnect track graph";
        }

        static std::optional<GraphConnectionSubject> resolve(
            PropertyCommandTarget&,
            const GraphConnectionAddress& address) {
            return GraphConnectionSubject{address};
        }

        static Value read(PropertyCommandTarget& target, const GraphConnectionSubject& subject) {
            return target.graphConnectionPresent(
                subject.address.trackReferenceId, subject.address.connection);
        }

        static bool write(
            PropertyCommandTarget& target,
            const GraphConnectionSubject& subject,
            const Value& value) {
            return target.applyGraphConnectionPresence(
                subject.address.trackReferenceId, subject.address.connection, value);
        }

        // A rejected connection says why -- a cycle, a missing endpoint, a
        // direction mismatch -- and that reason is what the user needs.
        static std::string failureMessage(PropertyCommandTarget& target) {
            return target.lastWriteFailure();
        }

        static void notify(PropertyCommandTarget&, const GraphConnectionSubject&) {
        }
    };

    struct LatencyCompensationSettingsProperty
        : ProjectPropertyDescriptor<
              LatencyCompensationSettingsProperty,
              LatencyCompensationProjectSettings> {
        static constexpr std::string_view commandId{"project.setLatencyCompensationSettings"};
        static constexpr std::string_view label{"Change latency compensation settings"};

        static Value read(PropertyCommandTarget& target, const ProjectSubject&) {
            return target.latencyCompensationSettings();
        }

        static bool equal(const Value& lhs, const Value& rhs) {
            return latencyCompensationSettingsEqual(lhs, rhs);
        }

        static bool write(
            PropertyCommandTarget& target,
            const ProjectSubject&,
            const Value& value) {
            return target.applyLatencyCompensationSettings(value);
        }

        // Rejected settings name what was wrong with them -- an unsupported
        // implementation id, or a malformed property map.
        static std::string failureMessage(PropertyCommandTarget& target) {
            return target.lastWriteFailure();
        }
    };

} // namespace uapmd::timeline_detail
