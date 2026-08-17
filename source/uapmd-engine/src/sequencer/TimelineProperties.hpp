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

    public:
        PropertyCommand(
            PropertyCommandTarget& target,
            Address address,
            Value value)
            : target_(target)
            , address_(std::move(address))
            , value_(std::move(value)) {
        }

        std::string_view commandId() const override {
            return Property::commandId;
        }

        std::string description() const override {
            return Property::describe(value_);
        }

        size_t retainedSizeInBytes() const override {
            return sizeof(*this)
                + retainedValueSize(address_)
                + retainedValueSize(value_);
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
                completion(ProjectCommandResult::failure(std::format(
                    "Could not apply '{}'.",
                    Property::commandId)));
                return;
            }
            Property::notify(target_, *subject);
            context.recordRevert(std::make_shared<PropertyCommand>(
                target_, address_, std::move(before)));
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

} // namespace uapmd::timeline_detail
