#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <remidy/detail/event-loop.hpp>
#include "ProjectSerialization.hpp"
#include "remidy/remidy.hpp"
#include "uapmd-data/uapmd-data.hpp"
#include "uapmd-plugin-hosting/uapmd-plugin-hosting.hpp"
#include "uapmd-engine/uapmd-engine.hpp"
#include <umppi/umppi.hpp>

using namespace uapmd_plugin_hosting;

namespace uapmd {

    namespace {
        struct PendingProjectPluginState {
            int32_t instance_id{-1};
            size_t plugin_order{0};
            AudioPluginInstanceAPI* instance{};
            std::function<void(const std::string& relativePath)> set_state_file{};
            std::string scope_label;
        };

        struct PendingProjectGraphSave {
            UapmdProjectTrackData* track{};
            SequencerTrack* sequencer_track{};
            std::string scope_label;
        };

        struct PendingProjectSaveContext {
            std::filesystem::path project_file;
            std::filesystem::path project_dir;
            std::filesystem::path plugin_state_dir;
            std::filesystem::path graph_dir;
            std::unique_ptr<UapmdProjectData> project;
            std::vector<PendingProjectPluginState> pending_states;
            std::vector<PendingProjectGraphSave> pending_graphs;
            size_t next_pending_state{0};
            uint64_t history_state_id{0};
            bool emit_document_event{true};
            TimelineFacade::ProjectSaveCallback callback;
        };

        size_t retainedValueSize(const std::string& value) {
            return sizeof(value) + value.capacity();
        }

        size_t retainedValueSize(const ClipMarker& value) {
            return sizeof(value)
                + value.markerId.capacity()
                + value.referenceClipId.capacity()
                + value.referenceMarkerId.capacity()
                + value.name.capacity();
        }

        size_t retainedValueSize(const AudioWarpPoint& value) {
            return sizeof(value)
                + value.referenceClipId.capacity()
                + value.referenceMarkerId.capacity();
        }

        size_t retainedValueSize(const TimeReference& value) {
            return sizeof(value) + value.referenceId.capacity();
        }

        template<typename Value>
        size_t retainedValueSize(const Value&) {
            return sizeof(Value);
        }

        template<typename Value>
        size_t retainedValueSize(const std::vector<Value>& values) {
            size_t result = sizeof(values);
            for (const auto& value : values)
                result += retainedValueSize(value);
            return result;
        }

        size_t retainedValueSize(const ProjectClipFragment& fragment) {
            size_t result = sizeof(fragment)
                + fragment.clip.referenceId.capacity()
                + fragment.clip.name.capacity()
                + fragment.clip.filepath.capacity()
                + fragment.clip.anchorReferenceId.capacity()
                + retainedValueSize(fragment.clip.markers)
                + retainedValueSize(fragment.clip.audioWarps)
                + fragment.umpEvents.capacity() * sizeof(uapmd_ump_t)
                + fragment.umpTickTimestamps.capacity() * sizeof(uint64_t)
                + fragment.tempoChanges.capacity() * sizeof(MidiTempoChange)
                + fragment.timeSignatureChanges.capacity() * sizeof(MidiTimeSignatureChange);
            for (const auto& [extensionId, state] : fragment.extensionState)
                result += sizeof(extensionId) + extensionId.capacity()
                    + sizeof(state) + state.capacity();
            return result;
        }

        size_t retainedValueSize(const ProjectTrackFragment& fragment) {
            size_t result = sizeof(fragment)
                + fragment.referenceId.capacity()
                + fragment.graphType.capacity()
                + fragment.graphBytes.capacity();
            for (const auto& plugin : fragment.plugins)
                result += sizeof(plugin)
                    + plugin.nodeId.capacity()
                    + plugin.pluginId.capacity()
                    + plugin.format.capacity()
                    + plugin.displayName.capacity()
                    + plugin.state.capacity();
            for (const auto& clip : fragment.clips)
                result += retainedValueSize(clip);
            for (const auto& [extensionId, state] : fragment.extensionState)
                result += sizeof(extensionId) + extensionId.capacity()
                    + sizeof(state) + state.capacity();
            return result;
        }

        size_t retainedValueSize(
            const LatencyCompensationProjectSettings& settings) {
            size_t result = sizeof(settings)
                + settings.implementation_id.capacity()
                + settings.monitored_track_indexes.capacity() * sizeof(int32_t)
                + settings.record_armed_track_indexes.capacity() * sizeof(int32_t);
            for (const auto& [key, value] : settings.implementation_properties)
                result += sizeof(key) + key.capacity()
                    + sizeof(value) + value.capacity();
            return result;
        }

        bool latencyCompensationSettingsEqual(
            const LatencyCompensationProjectSettings& lhs,
            const LatencyCompensationProjectSettings& rhs) {
            return lhs.implementation_id == rhs.implementation_id
                && lhs.playback_compensation_mode == rhs.playback_compensation_mode
                && lhs.input_monitoring_policy == rhs.input_monitoring_policy
                && lhs.monitored_track_indexes == rhs.monitored_track_indexes
                && lhs.record_armed_track_indexes == rhs.record_armed_track_indexes
                && lhs.implementation_properties == rhs.implementation_properties;
        }

        struct TrackGraphSnapshot {
            std::string graphType;
            std::vector<uint8_t> graphBytes;
        };

        size_t retainedValueSize(const TrackGraphSnapshot& snapshot) {
            return sizeof(snapshot)
                + snapshot.graphType.capacity()
                + snapshot.graphBytes.capacity();
        }

        bool clipMarkerEqual(const ClipMarker& lhs, const ClipMarker& rhs) {
            return lhs.markerId == rhs.markerId
                && lhs.clipPositionOffset == rhs.clipPositionOffset
                && lhs.referenceType == rhs.referenceType
                && lhs.referenceClipId == rhs.referenceClipId
                && lhs.referenceMarkerId == rhs.referenceMarkerId
                && lhs.name == rhs.name;
        }

        bool audioWarpPointEqual(const AudioWarpPoint& lhs, const AudioWarpPoint& rhs) {
            return lhs.clipPositionOffset == rhs.clipPositionOffset
                && lhs.speedRatio == rhs.speedRatio
                && lhs.referenceType == rhs.referenceType
                && lhs.referenceClipId == rhs.referenceClipId
                && lhs.referenceMarkerId == rhs.referenceMarkerId;
        }

        bool clipMarkersEqual(const std::vector<ClipMarker>& lhs, const std::vector<ClipMarker>& rhs) {
            return lhs.size() == rhs.size()
                && std::equal(lhs.begin(), lhs.end(), rhs.begin(), clipMarkerEqual);
        }

        bool audioWarpPointsEqual(
            const std::vector<AudioWarpPoint>& lhs,
            const std::vector<AudioWarpPoint>& rhs) {
            return lhs.size() == rhs.size()
                && std::equal(lhs.begin(), lhs.end(), rhs.begin(), audioWarpPointEqual);
        }

        template<typename Value>
        class ClipPropertyUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                std::string_view clipReferenceId,
                const Value& value)>;

            ClipPropertyUndoOperation(
                std::string description,
                std::string propertyKey,
                std::string trackReferenceId,
                std::string clipReferenceId,
                Value before,
                Value after,
                Apply apply,
                std::function<bool(const Value&, const Value&)> equal)
                : description_(std::move(description))
                , property_key_(std::move(propertyKey))
                , track_reference_id_(std::move(trackReferenceId))
                , clip_reference_id_(std::move(clipReferenceId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply))
                , equal_(std::move(equal)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + description_.capacity()
                    + property_key_.capacity()
                    + track_reference_id_.capacity()
                    + clip_reference_id_.capacity()
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            bool mergeWith(const ProjectUndoableOperation& subsequent) override {
                const auto* next = dynamic_cast<const ClipPropertyUndoOperation*>(&subsequent);
                if (!next
                    || property_key_ != next->property_key_
                    || track_reference_id_ != next->track_reference_id_
                    || clip_reference_id_ != next->clip_reference_id_)
                    return false;
                description_ = next->description_;
                after_ = next->after_;
                apply_ = next->apply_;
                equal_ = next->equal_;
                return true;
            }

            bool hasEffect() const override {
                return !equal_ || !equal_(before_, after_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

        private:
            void apply(const Value& value, ProjectUndoCompletion completion) {
                if (apply_ && apply_(track_reference_id_, clip_reference_id_, value)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(std::format(
                        "Could not apply '{}' to clip {} on track {}.",
                        description_, clip_reference_id_, track_reference_id_)));
            }

            std::string description_{};
            std::string property_key_{};
            std::string track_reference_id_{};
            std::string clip_reference_id_{};
            Value before_{};
            Value after_{};
            Apply apply_{};
            std::function<bool(const Value&, const Value&)> equal_{};
        };

        template<typename Value>
        class TrackPropertyUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                const Value& value)>;

            TrackPropertyUndoOperation(
                std::string description,
                std::string propertyKey,
                std::string trackReferenceId,
                Value before,
                Value after,
                Apply apply)
                : description_(std::move(description))
                , property_key_(std::move(propertyKey))
                , track_reference_id_(std::move(trackReferenceId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + description_.capacity()
                    + property_key_.capacity()
                    + track_reference_id_.capacity()
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            bool mergeWith(const ProjectUndoableOperation& subsequent) override {
                const auto* next = dynamic_cast<const TrackPropertyUndoOperation*>(&subsequent);
                if (!next
                    || property_key_ != next->property_key_
                    || track_reference_id_ != next->track_reference_id_)
                    return false;
                description_ = next->description_;
                after_ = next->after_;
                apply_ = next->apply_;
                return true;
            }

            bool hasEffect() const override {
                return before_ != after_;
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

        private:
            void apply(const Value& value, ProjectUndoCompletion completion) {
                if (apply_ && apply_(track_reference_id_, value)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(std::format(
                        "Could not apply '{}' to track {}.",
                        description_, track_reference_id_)));
            }

            std::string description_{};
            std::string property_key_{};
            std::string track_reference_id_{};
            Value before_{};
            Value after_{};
            Apply apply_{};
        };

        class LatencySettingsUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                const LatencyCompensationProjectSettings& settings,
                std::string& error)>;

            LatencySettingsUndoOperation(
                LatencyCompensationProjectSettings before,
                LatencyCompensationProjectSettings after,
                Apply apply)
                : before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return "Change latency compensation settings";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            bool hasEffect() const override {
                return !latencyCompensationSettingsEqual(before_, after_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

        private:
            void apply(
                const LatencyCompensationProjectSettings& settings,
                ProjectUndoCompletion completion) {
                std::string error;
                if (apply_ && apply_(settings, error)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        error.empty()
                            ? "Could not apply latency compensation settings"
                            : std::move(error)));
            }

            LatencyCompensationProjectSettings before_{};
            LatencyCompensationProjectSettings after_{};
            Apply apply_{};
        };

        class DeviceInputUndoOperation final : public ProjectUndoableOperation {
        public:
            using Channels = std::optional<std::vector<uint32_t>>;
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                int32_t sourceNodeId,
                const Channels& channels)>;

            DeviceInputUndoOperation(
                std::string description,
                std::string trackReferenceId,
                int32_t sourceNodeId,
                Channels before,
                Channels after,
                Apply apply)
                : description_(std::move(description))
                , track_reference_id_(std::move(trackReferenceId))
                , source_node_id_(sourceNodeId)
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + description_.capacity()
                    + track_reference_id_.capacity()
                    + (before_ ? retainedValueSize(*before_) : 0)
                    + (after_ ? retainedValueSize(*after_) : 0);
            }

            bool hasEffect() const override {
                return before_ != after_;
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

        private:
            void apply(const Channels& channels, ProjectUndoCompletion completion) {
                if (apply_ && apply_(track_reference_id_, source_node_id_, channels)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(std::format(
                        "Could not apply '{}' to device input {} on track {}.",
                        description_, source_node_id_, track_reference_id_)));
            }

            std::string description_{};
            std::string track_reference_id_{};
            int32_t source_node_id_{-1};
            Channels before_{};
            Channels after_{};
            Apply apply_{};
        };

        template<typename Value>
        class PluginPropertyUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                std::string_view nodeId,
                const Value& value)>;

            PluginPropertyUndoOperation(
                std::string description,
                std::string propertyKey,
                std::string trackReferenceId,
                std::string nodeId,
                Value before,
                Value after,
                Apply apply)
                : description_(std::move(description))
                , property_key_(std::move(propertyKey))
                , track_reference_id_(std::move(trackReferenceId))
                , node_id_(std::move(nodeId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + description_.capacity()
                    + property_key_.capacity()
                    + track_reference_id_.capacity()
                    + node_id_.capacity()
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            bool mergeWith(const ProjectUndoableOperation& subsequent) override {
                const auto* next = dynamic_cast<const PluginPropertyUndoOperation*>(&subsequent);
                if (!next
                    || property_key_ != next->property_key_
                    || track_reference_id_ != next->track_reference_id_
                    || node_id_ != next->node_id_)
                    return false;
                description_ = next->description_;
                after_ = next->after_;
                apply_ = next->apply_;
                return true;
            }

            bool hasEffect() const override {
                return before_ != after_;
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

        private:
            void apply(const Value& value, ProjectUndoCompletion completion) {
                if (apply_ && apply_(track_reference_id_, node_id_, value)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(std::format(
                        "Could not apply '{}' to plug-in node {} on track {}.",
                        description_, node_id_, track_reference_id_)));
            }

            std::string description_{};
            std::string property_key_{};
            std::string track_reference_id_{};
            std::string node_id_{};
            Value before_{};
            Value after_{};
            Apply apply_{};
        };

        class GraphConnectionUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                const uapmd_graph::AudioPluginGraphConnection& connection,
                bool present,
                std::string& error)>;

            GraphConnectionUndoOperation(
                bool initialAddition,
                std::string trackReferenceId,
                uapmd_graph::AudioPluginGraphConnection connection,
                Apply apply)
                : initial_addition_(initialAddition)
                , track_reference_id_(std::move(trackReferenceId))
                , connection_(std::move(connection))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return initial_addition_
                    ? "Connect track graph"
                    : "Disconnect track graph";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + track_reference_id_.capacity()
                    + connection_.source.node_id.capacity()
                    + connection_.target.node_id.capacity();
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(initial_addition_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(!initial_addition_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                perform(context, std::move(completion));
            }

        private:
            void apply(bool present, ProjectUndoCompletion completion) {
                std::string error;
                if (apply_
                    && apply_(track_reference_id_, connection_, present, error)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        error.empty()
                            ? "Could not change the track graph connection"
                            : std::move(error)));
            }

            bool initial_addition_{false};
            std::string track_reference_id_{};
            uapmd_graph::AudioPluginGraphConnection connection_{};
            Apply apply_{};
        };

        class TrackGraphUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                const TrackGraphSnapshot& desired,
                const TrackGraphSnapshot& compensation)>;

            TrackGraphUndoOperation(
                std::string trackReferenceId,
                TrackGraphSnapshot before,
                TrackGraphSnapshot after,
                Apply apply)
                : track_reference_id_(std::move(trackReferenceId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return "Change track graph type";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + track_reference_id_.capacity()
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, before_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, after_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                perform(context, std::move(completion));
            }

        private:
            void apply(
                const TrackGraphSnapshot& desired,
                const TrackGraphSnapshot& compensation,
                ProjectUndoCompletion completion) {
                if (apply_
                    && apply_(track_reference_id_, desired, compensation)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "Could not restore the track graph"));
            }

            std::string track_reference_id_{};
            TrackGraphSnapshot before_{};
            TrackGraphSnapshot after_{};
            Apply apply_{};
        };

        class PluginStateUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<void(
                std::string_view trackReferenceId,
                std::string_view nodeId,
                const std::vector<uint8_t>& state,
                ProjectUndoCompletion completion)>;

            PluginStateUndoOperation(
                std::string trackReferenceId,
                std::string nodeId,
                std::vector<uint8_t> before,
                std::vector<uint8_t> after,
                Apply apply)
                : track_reference_id_(std::move(trackReferenceId))
                , node_id_(std::move(nodeId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return "Load plug-in state";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + track_reference_id_.capacity()
                    + node_id_.capacity()
                    + before_.capacity()
                    + after_.capacity();
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                perform(context, std::move(completion));
            }

        private:
            void apply(
                const std::vector<uint8_t>& state,
                ProjectUndoCompletion completion) {
                if (apply_) {
                    apply_(track_reference_id_, node_id_, state,
                           std::move(completion));
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "Could not resolve the plug-in state mutation"));
            }

            std::string track_reference_id_{};
            std::string node_id_{};
            std::vector<uint8_t> before_{};
            std::vector<uint8_t> after_{};
            Apply apply_{};
        };

        class ClipRemovalUndoOperation final : public ProjectUndoableOperation {
        public:
            using Remove = std::function<bool(
                std::string_view trackReferenceId,
                std::string_view clipReferenceId)>;
            using Restore = std::function<TimelineFacade::ClipAddResult(
                std::string_view trackReferenceId,
                const ProjectClipFragment& fragment)>;

            ClipRemovalUndoOperation(
                std::string trackReferenceId,
                ProjectClipFragment fragment,
                Remove remove,
                Restore restore)
                : track_reference_id_(std::move(trackReferenceId))
                , fragment_(std::move(fragment))
                , remove_(std::move(remove))
                , restore_(std::move(restore)) {
            }

            std::string description() const override {
                return "Delete clip";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + track_reference_id_.capacity()
                    + retainedValueSize(fragment_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                remove(std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                if (!restore_) {
                    completeFailure(std::move(completion), "Clip restoration is unavailable.");
                    return;
                }
                auto result = restore_(track_reference_id_, fragment_);
                if (!result.success) {
                    completeFailure(
                        std::move(completion),
                        result.error.empty() ? "Could not restore the deleted clip." : std::move(result.error));
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::success());
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                remove(std::move(completion));
            }

        private:
            void remove(ProjectUndoCompletion completion) {
                if (remove_ && remove_(track_reference_id_, fragment_.clip.referenceId)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                completeFailure(std::move(completion), "Could not remove the clip.");
            }

            void completeFailure(ProjectUndoCompletion completion, std::string message) const {
                if (completion)
                    completion(ProjectUndoResult::failure(
                        std::move(message) + " Clip " + fragment_.clip.referenceId
                        + " on track " + track_reference_id_ + "."));
            }

            std::string track_reference_id_{};
            ProjectClipFragment fragment_{};
            Remove remove_{};
            Restore restore_{};
        };

        class ClipAdditionUndoOperation final : public ProjectUndoableOperation {
        public:
            using Remove = ClipRemovalUndoOperation::Remove;
            using Restore = ClipRemovalUndoOperation::Restore;

            ClipAdditionUndoOperation(
                std::string trackReferenceId,
                ProjectClipFragment fragment,
                Remove remove,
                Restore restore)
                : track_reference_id_(std::move(trackReferenceId))
                , fragment_(std::move(fragment))
                , remove_(std::move(remove))
                , restore_(std::move(restore)) {
            }

            std::string description() const override {
                return "Add clip";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + track_reference_id_.capacity()
                    + retainedValueSize(fragment_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                restore(std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                if (remove_ && remove_(track_reference_id_, fragment_.clip.referenceId)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                completeFailure(std::move(completion), "Could not remove the added clip.");
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                restore(std::move(completion));
            }

        private:
            void restore(ProjectUndoCompletion completion) {
                if (!restore_) {
                    completeFailure(std::move(completion), "Clip restoration is unavailable.");
                    return;
                }
                auto result = restore_(track_reference_id_, fragment_);
                if (!result.success) {
                    completeFailure(
                        std::move(completion),
                        result.error.empty() ? "Could not restore the added clip." : std::move(result.error));
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::success());
            }

            void completeFailure(ProjectUndoCompletion completion, std::string message) const {
                if (completion)
                    completion(ProjectUndoResult::failure(
                        std::move(message) + " Clip " + fragment_.clip.referenceId
                        + " on track " + track_reference_id_ + "."));
            }

            std::string track_reference_id_{};
            ProjectClipFragment fragment_{};
            Remove remove_{};
            Restore restore_{};
        };

        class ClipContentUndoOperation final : public ProjectUndoableOperation {
        public:
            using Apply = std::function<bool(
                std::string_view trackReferenceId,
                const ProjectClipFragment& desired,
                const ProjectClipFragment& compensation)>;

            ClipContentUndoOperation(
                std::string description,
                std::string trackReferenceId,
                ProjectClipFragment before,
                ProjectClipFragment after,
                Apply apply)
                : description_(std::move(description))
                , track_reference_id_(std::move(trackReferenceId))
                , before_(std::move(before))
                , after_(std::move(after))
                , apply_(std::move(apply)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this)
                    + description_.capacity()
                    + track_reference_id_.capacity()
                    + retainedValueSize(before_)
                    + retainedValueSize(after_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, before_, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(before_, after_, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                apply(after_, before_, std::move(completion));
            }

        private:
            void apply(
                const ProjectClipFragment& desired,
                const ProjectClipFragment& compensation,
                ProjectUndoCompletion completion) {
                if (apply_ && apply_(track_reference_id_, desired, compensation)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(std::format(
                        "Could not apply '{}' to clip {} on track {}.",
                        description_, desired.clip.referenceId, track_reference_id_)));
            }

            std::string description_{};
            std::string track_reference_id_{};
            ProjectClipFragment before_{};
            ProjectClipFragment after_{};
            Apply apply_{};
        };

        class TrackStructureUndoOperation final : public ProjectUndoableOperation {
        public:
            enum class InitialDirection {
                Addition,
                Removal
            };
            using Remove = std::function<bool(std::string_view trackReferenceId)>;
            using Restore = std::function<void(
                const ProjectTrackFragment& fragment,
                int32_t insertionIndex,
                ProjectUndoCompletion completion)>;

            TrackStructureUndoOperation(
                InitialDirection initialDirection,
                int32_t insertionIndex,
                ProjectTrackFragment fragment,
                Remove remove,
                Restore restore)
                : initial_direction_(initialDirection)
                , insertion_index_(insertionIndex)
                , fragment_(std::move(fragment))
                , remove_(std::move(remove))
                , restore_(std::move(restore)) {
            }

            std::string description() const override {
                return initial_direction_ == InitialDirection::Addition
                    ? "Add track"
                    : "Delete track";
            }

            size_t historySizeInBytes() const override {
                return sizeof(*this) + retainedValueSize(fragment_);
            }

            void perform(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                if (initial_direction_ == InitialDirection::Addition)
                    restore(std::move(completion));
                else
                    remove(std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext&,
                ProjectUndoCompletion completion) override {
                if (initial_direction_ == InitialDirection::Addition)
                    remove(std::move(completion));
                else
                    restore(std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                perform(context, std::move(completion));
            }

        private:
            void remove(ProjectUndoCompletion completion) {
                if (remove_ && remove_(fragment_.referenceId)) {
                    if (completion)
                        completion(ProjectUndoResult::success());
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "Could not remove track " + fragment_.referenceId + "."));
            }

            void restore(ProjectUndoCompletion completion) {
                if (restore_) {
                    restore_(fragment_, insertion_index_, std::move(completion));
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "Could not restore track " + fragment_.referenceId + "."));
            }

            InitialDirection initial_direction_{};
            int32_t insertion_index_{-1};
            ProjectTrackFragment fragment_{};
            Remove remove_{};
            Restore restore_{};
        };
    } // namespace

    class TimelineFacadeImpl : public TimelineFacade,
                               public ProjectDocumentView,
                               public PluginInstanceLifecycleListener {
        SequencerEngine& engine_;
        int32_t sampleRate_;
        uint32_t bufferSizeInFrames_;

        using TrackList = std::vector<std::shared_ptr<TimelineTrack>>;
        TrackList timeline_tracks_;                          // UI-thread owned
        std::shared_ptr<const TrackList> timeline_tracks_snapshot_; // RT-thread read via atomic
        std::shared_ptr<TimelineTrack> master_timeline_track_;

        // Propagates the master track's tempo/time-signature authority to every regular-track
        // MIDI clip (see MidiClipReader::applyAuthoritativeTempoMapToMusicalClips).
        void applyAuthoritativeTempoMapToMusicalClips() {
            auto tempoChanges = MidiClipReader::applyAuthoritativeTempoMapToMusicalClips(master_timeline_track_, timeline_tracks_);
            if (!tempoChanges.empty())
                timeline_.tempo = tempoChanges.front().bpm;
        }

        void resolveAllClipAnchors() {
            struct ClipRecord {
                ClipManager* manager{};
                ClipData clip{};
            };
            std::unordered_map<std::string, ClipRecord> records;
            auto collect = [&records](TimelineTrack* track) {
                if (!track)
                    return;
                for (const auto& clip : track->clipManager().getAllClips())
                    records.emplace(
                        clip.referenceId,
                        ClipRecord{&track->clipManager(), clip});
            };
            collect(master_timeline_track_.get());
            for (const auto& track : timeline_tracks_)
                collect(track.get());

            std::unordered_map<std::string, TimelinePosition> resolved;
            std::unordered_set<std::string> resolving;
            std::function<TimelinePosition(const std::string&)> resolve =
                [&](const std::string& referenceId) -> TimelinePosition {
                    if (auto found = resolved.find(referenceId); found != resolved.end())
                        return found->second;
                    auto found = records.find(referenceId);
                    if (found == records.end())
                        return {};

                    const auto reference = found->second.clip.timeReference(sampleRate_);
                    if (reference.referenceId.empty()) {
                        auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                        resolved[referenceId] = position;
                        return position;
                    }
                    if (!resolving.insert(referenceId).second) {
                        auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                        resolved[referenceId] = position;
                        return position;
                    }

                    auto anchor = records.find(reference.referenceId);
                    if (anchor == records.end()) {
                        resolving.erase(referenceId);
                        auto position = TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                        resolved[referenceId] = position;
                        return position;
                    }
                    auto position = resolve(reference.referenceId);
                    if (reference.type == TimeReferenceType::ContainerEnd)
                        position.samples += anchor->second.clip.durationSamples;
                    position = position
                        + TimelinePosition::fromSeconds(reference.offset, sampleRate_);
                    resolving.erase(referenceId);
                    resolved[referenceId] = position;
                    return position;
                };

            for (const auto& [referenceId, record] : records)
                record.manager->setClipPosition(record.clip.clipId, resolve(referenceId));
        }

        static void appendMidiNodeMetaToSnapshot(MasterTrackSnapshot& snapshot,
                                                 const ClipData& clip,
                                                 MidiClipSourceNode& midiNode,
                                                 double sampleRate) {
            const double clipStartSamples = static_cast<double>(clip.position.samples);

            const auto& tempoSamples = midiNode.tempoChangeSamples();
            const auto& tempoEvents = midiNode.tempoChanges();
            const size_t tempoCount = std::min(tempoSamples.size(), tempoEvents.size());
            for (size_t i = 0; i < tempoCount; ++i) {
                MasterTrackSnapshot::TempoPoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(tempoSamples[i])) / sampleRate;
                point.tickPosition = tempoEvents[i].tickPosition;
                point.bpm = tempoEvents[i].bpm;
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.tempoPoints.push_back(point);
            }

            const auto& sigSamples = midiNode.timeSignatureChangeSamples();
            const auto& sigEvents = midiNode.timeSignatureChanges();
            const size_t sigCount = std::min(sigSamples.size(), sigEvents.size());
            for (size_t i = 0; i < sigCount; ++i) {
                MasterTrackSnapshot::TimeSignaturePoint point;
                point.timeSeconds = (clipStartSamples + static_cast<double>(sigSamples[i])) / sampleRate;
                point.tickPosition = sigEvents[i].tickPosition;
                point.signature = sigEvents[i];
                snapshot.maxTimeSeconds = std::max(snapshot.maxTimeSeconds, point.timeSeconds);
                snapshot.timeSignaturePoints.push_back(point);
            }
        }

        void rebuildTrackSnapshot() {
            auto snap = std::make_shared<TrackList>(timeline_tracks_);
            std::atomic_store_explicit(&timeline_tracks_snapshot_,
                std::shared_ptr<const TrackList>(snap), std::memory_order_release);
        }

        TimelineState timeline_;
        int32_t next_source_node_id_{1};
        uint32_t next_timeline_track_reference_{1};
        std::function<void()> timeline_changed_callback_{};
        bool suppress_timeline_notification_{false};
        bool suppress_project_document_events_{false};
        AudioGraphProviderRegistry audio_graph_provider_registry_{};
        ProjectDocumentEventDispatcher project_document_events_{};
        ProjectUndoEngine undo_engine_{{
            .maximumHistorySizeInBytes = 64u * 1024u * 1024u,
            .dispatchToModelThread = [](ProjectUndoTask task) {
                if (!task)
                    return;
                if (remidy::EventLoop::runningOnMainThread())
                    task();
                else
                    remidy::EventLoop::enqueueTaskOnMainThread(std::move(task));
            }
        }};
        std::shared_ptr<AudioSourceRepository> audio_source_repository_{std::make_shared<FileAudioSourceRepository>()};
        mutable std::mutex project_serialization_extensions_mutex_{};
        std::vector<ProjectSerializationExtension*> project_serialization_extensions_{};
        std::unordered_map<int32_t, std::unordered_map<int32_t, double>> plugin_parameter_values_{};
        std::unordered_map<int32_t, remidy::EventListenerId> plugin_parameter_listener_ids_{};
        std::atomic<uint32_t> plugin_parameter_mutation_depth_{0};

        // Identity staged by the project loader for the next track or clip it
        // creates, so that loaded objects keep the reference IDs they were
        // saved under instead of being given freshly allocated ones. Empty
        // means "allocate as usual"; each is consumed by the next creation.
        std::string pending_track_reference_id_{};
        std::string pending_clip_reference_id_{};

        // Copied under the lock so that extensions can be invoked without
        // holding it; an extension may register or unregister another.
        std::vector<ProjectSerializationExtension*> projectSerializationExtensionsSnapshot() const {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            return project_serialization_extensions_;
        }

        // Resolves a track index to its TimelineTrack, mapping
        // kMasterTrackIndex onto the master track. Returns nullptr when the
        // index addresses no track.
        TimelineTrack* resolveTrack(int32_t trackIndex) {
            if (trackIndex == kMasterTrackIndex)
                return master_timeline_track_.get();
            if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size()))
                return timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            return nullptr;
        }

        const TimelineTrack* resolveTrack(int32_t trackIndex) const {
            return const_cast<TimelineFacadeImpl*>(this)->resolveTrack(trackIndex);
        }

        TimelineTrack* resolveTrackByReferenceId(std::string_view trackReferenceId) {
            if (master_timeline_track_ && master_timeline_track_->referenceId() == trackReferenceId)
                return master_timeline_track_.get();
            for (const auto& track : timeline_tracks_)
                if (track && track->referenceId() == trackReferenceId)
                    return track.get();
            return nullptr;
        }

        static int32_t clipIdForReferenceId(
            const TimelineTrack& track,
            std::string_view clipReferenceId) {
            for (const auto& clip : track.clipManager().getAllClips())
                if (clip.referenceId == clipReferenceId)
                    return clip.clipId;
            return -1;
        }

        int32_t trackIndexForPersistentId(std::string_view trackReferenceId) const {
            if (master_timeline_track_ && master_timeline_track_->referenceId() == trackReferenceId)
                return kMasterTrackIndex;
            for (size_t index = 0; index < timeline_tracks_.size(); ++index)
                if (timeline_tracks_[index]
                    && timeline_tracks_[index]->referenceId() == trackReferenceId)
                    return static_cast<int32_t>(index);
            return -1;
        }

        bool removeClipRaw(TimelineTrack& targetTrack, int32_t clipId) {
            const auto* clip = targetTrack.clipManager().getClip(clipId);
            if (!clip)
                return false;
            auto removedClip = *clip;
            if (!targetTrack.removeClip(clipId))
                return false;
            applyAuthoritativeTempoMapToMusicalClips();
            emitClipRemoved(targetTrack, removedClip);
            if (removedClip.clipType == ClipType::Midi)
                emitMasterTrackChanged("master-track-content-changed");
            notifyTimelineChanged();
            return true;
        }

        bool removeClipByReferenceId(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId) {
            auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
            if (!targetTrack)
                return false;
            const auto clipId = clipIdForReferenceId(*targetTrack, clipReferenceId);
            return clipId >= 0 && removeClipRaw(*targetTrack, clipId);
        }

        ClipAddResult restoreClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& fragment) {
            const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
            if (trackIndex < 0 && trackIndex != kMasterTrackIndex)
                return {.error = "The clip's track no longer exists"};
            return attachClipFragment(trackIndex, fragment, ProjectObjectIdPolicy::Restore);
        }

        bool replaceClipByReferenceId(
            std::string_view trackReferenceId,
            const ProjectClipFragment& desired,
            const ProjectClipFragment* compensation) {
            auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
            if (!targetTrack)
                return false;
            const auto currentClipId =
                clipIdForReferenceId(*targetTrack, desired.clip.referenceId);
            if (currentClipId < 0 || !removeClipRaw(*targetTrack, currentClipId))
                return false;
            auto restored = restoreClipByReferenceId(trackReferenceId, desired);
            if (restored.success) {
                resolveAllClipAnchors();
                return true;
            }
            if (compensation) {
                const auto compensated =
                    restoreClipByReferenceId(trackReferenceId, *compensation);
                if (compensated.success)
                    resolveAllClipAnchors();
            }
            return false;
        }

        bool recordReplacedClip(
            int32_t trackIndex,
            ProjectClipFragment before,
            ProjectMutationOrigin origin,
            std::string description) {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            auto after = captureClipFragment(trackIndex, before.clip.clipId);
            if (!after) {
                replaceClipByReferenceId(targetTrack->referenceId(), before, nullptr);
                return false;
            }
            auto operation = std::make_shared<ClipContentUndoOperation>(
                std::move(description),
                targetTrack->referenceId(),
                before,
                *after,
                [this](std::string_view persistentTrackId,
                       const ProjectClipFragment& desired,
                       const ProjectClipFragment& compensation) {
                    return replaceClipByReferenceId(
                        persistentTrackId, desired, &compensation);
                });
            auto recorded = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.recordPerformed(
                std::move(operation),
                origin,
                [recorded](ProjectUndoResult result) {
                    *recorded = std::move(result);
                });
            if (recorded->has_value() && recorded->value().succeeded())
                return true;
            replaceClipByReferenceId(targetTrack->referenceId(), before, &*after);
            return false;
        }

        bool performCapturedClipRemoval(
            std::string trackReferenceId,
            ProjectClipFragment fragment,
            ProjectMutationOrigin origin) {
            if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote)
                return removeClipByReferenceId(trackReferenceId, fragment.clip.referenceId);

            auto operation = std::make_shared<ClipRemovalUndoOperation>(
                std::move(trackReferenceId),
                std::move(fragment),
                [this](std::string_view persistentTrackId, std::string_view persistentClipId) {
                    return removeClipByReferenceId(persistentTrackId, persistentClipId);
                },
                [this](std::string_view persistentTrackId, const ProjectClipFragment& captured) {
                    return restoreClipByReferenceId(persistentTrackId, captured);
                });
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        ClipAddResult recordAddedClip(
            int32_t trackIndex,
            ClipAddResult result,
            ProjectMutationOrigin origin) {
            if (!result.success
                || (origin != ProjectMutationOrigin::User
                    && origin != ProjectMutationOrigin::Remote))
                return result;

            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack) {
                result.success = false;
                result.error = "The added clip's track no longer exists";
                return result;
            }

            // The persistent identity and extension-owned state do not exist
            // until construction succeeds. Capture immediately afterwards,
            // outside a document transaction, then register the already-
            // performed operation. If either step fails, remove the new clip
            // so an untracked user mutation never leaks into the document.
            auto fragment = captureClipFragment(trackIndex, result.clipId);
            if (!fragment) {
                removeClipRaw(*targetTrack, result.clipId);
                result.success = false;
                result.error = "Could not capture the added clip for undo history";
                return result;
            }

            auto operation = std::make_shared<ClipAdditionUndoOperation>(
                targetTrack->referenceId(),
                std::move(*fragment),
                [this](std::string_view persistentTrackId, std::string_view persistentClipId) {
                    return removeClipByReferenceId(persistentTrackId, persistentClipId);
                },
                [this](std::string_view persistentTrackId, const ProjectClipFragment& captured) {
                    return restoreClipByReferenceId(persistentTrackId, captured);
                });
            auto recorded = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.recordPerformed(
                std::move(operation),
                origin,
                [recorded](ProjectUndoResult completed) {
                    *recorded = std::move(completed);
                });
            if (recorded->has_value() && recorded->value().succeeded())
                return result;

            removeClipRaw(*targetTrack, result.clipId);
            result.success = false;
            result.error = recorded->has_value() && !recorded->value().error.empty()
                ? recorded->value().error
                : "Could not record the added clip in undo history";
            return result;
        }

        static void dispatchToModelThread(ProjectUndoTask task) {
            if (!task)
                return;
            if (remidy::EventLoop::runningOnMainThread())
                task();
            else
                remidy::EventLoop::enqueueTaskOnMainThread(std::move(task));
        }

        std::shared_ptr<ProjectUndoableOperation> makeTrackStructureOperation(
            TrackStructureUndoOperation::InitialDirection initialDirection,
            int32_t insertionIndex,
            ProjectTrackFragment fragment) {
            return std::make_shared<TrackStructureUndoOperation>(
                initialDirection,
                insertionIndex,
                std::move(fragment),
                [this](std::string_view trackReferenceId) {
                    const auto currentIndex = trackIndexForPersistentId(trackReferenceId);
                    return currentIndex >= 0 && engine_.removeTrack(currentIndex);
                },
                [this](const ProjectTrackFragment& captured,
                       int32_t restoreIndex,
                       ProjectUndoCompletion completion) {
                    ProjectTrackAttachOptions options;
                    options.idPolicy = ProjectObjectIdPolicy::Restore;
                    options.insertionIndex = restoreIndex;
                    attachTrackFragment(
                        captured,
                        options,
                        [completion = std::move(completion)](
                            int32_t attachedIndex,
                            std::string error) mutable {
                            if (!completion)
                                return;
                            if (attachedIndex >= 0 && error.empty()) {
                                completion(ProjectUndoResult::success());
                                return;
                            }
                            completion(ProjectUndoResult::failure(
                                error.empty() ? "Could not restore the track" : std::move(error)));
                        });
                });
        }

        template<typename Mutation>
        bool mutateClipByReferenceId(
            std::string_view trackReferenceId,
            std::string_view clipReferenceId,
            std::string changeType,
            Mutation&& mutate) {
            auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
            if (!targetTrack)
                return false;
            const auto clipId = clipIdForReferenceId(*targetTrack, clipReferenceId);
            if (clipId < 0 || !mutate(targetTrack->clipManager(), clipId))
                return false;
            auto* clip = targetTrack->clipManager().getClip(clipId);
            if (!clip)
                return false;
            emitClipChanged(*targetTrack, *clip, std::move(changeType));
            if (clip->clipType == ClipType::Midi)
                emitMasterTrackChanged("master-track-content-changed");
            notifyTimelineChanged();
            return true;
        }

        template<typename Value, typename Read, typename Mutation, typename Equal = std::equal_to<Value>>
        bool setUndoableClipProperty(
            int32_t trackIndex,
            int32_t clipId,
            Value after,
            ProjectMutationOrigin origin,
            std::string description,
            std::string changeType,
            Read&& read,
            Mutation&& mutate,
            Equal&& equal = {}) {
            auto* targetTrack = resolveTrack(trackIndex);
            auto* clip = targetTrack ? targetTrack->clipManager().getClip(clipId) : nullptr;
            if (!targetTrack || !clip)
                return false;

            Value before = read(*clip);
            if (equal(before, after))
                return true;

            auto trackReferenceId = targetTrack->referenceId();
            auto clipReferenceId = clip->referenceId;
            auto propertyKey = changeType;
            auto apply = [this,
                          changeType = std::move(changeType),
                          mutate = std::forward<Mutation>(mutate)](
                             std::string_view persistentTrackId,
                             std::string_view persistentClipId,
                             const Value& value) mutable {
                return mutateClipByReferenceId(
                    persistentTrackId,
                    persistentClipId,
                    changeType,
                    [&](ClipManager& clips, int32_t currentClipId) {
                        return mutate(clips, currentClipId, value);
                    });
            };

            if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote)
                return apply(trackReferenceId, clipReferenceId, after);

            auto operation = std::make_shared<ClipPropertyUndoOperation<Value>>(
                std::move(description),
                std::move(propertyKey),
                std::move(trackReferenceId),
                std::move(clipReferenceId),
                std::move(before),
                std::move(after),
                std::move(apply),
                std::forward<Equal>(equal));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            // Clip property operations complete inline. Keeping the result in
            // shared storage also makes an accidental off-thread call safe:
            // it reports false rather than leaving a dangling callback.
            return result->has_value() && result->value().succeeded();
        }

        SequencerTrack* resolveSequencerTrackByReferenceId(
            std::string_view trackReferenceId) {
            const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
            if (trackIndex == kMasterTrackIndex)
                return engine_.masterTrack();
            auto& tracks = engine_.tracks();
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(tracks.size()))
                return nullptr;
            return tracks[static_cast<size_t>(trackIndex)];
        }

        void emitTrackChanged(
            std::string_view trackReferenceId,
            std::string changeType) {
            const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
            if (trackIndex == kMasterTrackIndex) {
                emitMasterTrackChanged(std::move(changeType));
                return;
            }
            ProjectDocumentEvent event(
                ProjectDocumentEventKind::TrackChanged,
                std::move(changeType));
            event.setTrackId(std::string(trackReferenceId))
                .setTrackIndex(trackIndex);
            emitProjectDocumentEvent(std::move(event));
        }

        template<typename Value, typename Read, typename Mutation>
        bool setUndoableTrackProperty(
            int32_t trackIndex,
            Value after,
            ProjectMutationOrigin origin,
            std::string description,
            std::string changeType,
            Read&& read,
            Mutation&& mutate) {
            auto* timelineTrack = resolveTrack(trackIndex);
            auto* sequencerTrack = trackIndex == kMasterTrackIndex
                ? engine_.masterTrack()
                : (trackIndex >= 0
                    && trackIndex < static_cast<int32_t>(engine_.tracks().size())
                    ? engine_.tracks()[static_cast<size_t>(trackIndex)]
                    : nullptr);
            if (!timelineTrack || !sequencerTrack)
                return false;

            Value before = read(*sequencerTrack);
            if (before == after)
                return true;

            auto trackReferenceId = timelineTrack->referenceId();
            auto propertyKey = changeType;
            auto apply = [this,
                          changeType = std::move(changeType),
                          mutate = std::forward<Mutation>(mutate)](
                             std::string_view persistentTrackId,
                             const Value& value) mutable {
                auto* currentTrack =
                    resolveSequencerTrackByReferenceId(persistentTrackId);
                if (!currentTrack || !mutate(*currentTrack, value))
                    return false;
                emitTrackChanged(persistentTrackId, changeType);
                notifyTimelineChanged();
                return true;
            };

            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return apply(trackReferenceId, after);

            auto operation = std::make_shared<TrackPropertyUndoOperation<Value>>(
                std::move(description),
                std::move(propertyKey),
                std::move(trackReferenceId),
                std::move(before),
                std::move(after),
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        std::string takePendingClipReferenceId() {
            auto id = std::move(pending_clip_reference_id_);
            // A moved-from std::string is valid but unspecified, and short
            // strings are commonly left intact. Clear explicitly so a staged
            // identity cannot be handed to a second clip.
            pending_clip_reference_id_.clear();
            return id;
        }

        // Keeps the allocator ahead of every restored identifier, so that a
        // track added after a load cannot collide with one the project already
        // used.
        void reserveTrackReferenceId(std::string_view referenceId) {
            constexpr std::string_view prefix{"track_"};
            if (!referenceId.starts_with(prefix))
                return;
            uint32_t value{};
            auto digits = referenceId.substr(prefix.size());
            auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (ec != std::errc{} || ptr != digits.data() + digits.size())
                return;
            if (value >= next_timeline_track_reference_)
                next_timeline_track_reference_ = value + 1;
        }

    public:
        explicit TimelineFacadeImpl(SequencerEngine& engine)
            : engine_(engine)
            , sampleRate_(0)
            , bufferSizeInFrames_(0)
            , master_timeline_track_(std::make_shared<TimelineTrack>(std::string("master_track"), 0, 48000.0, 0))
        {
            engine_.addPluginInstanceLifecycleListener(*this);
            audio_graph_provider_registry_ = AudioGraphProviderRegistry::create();
            timeline_.tempo = 120.0;
            timeline_.timeSignatureNumerator = 4;
            timeline_.timeSignatureDenominator = 4;
            timeline_.isPlaying = false;
            timeline_.loopEnabled = false;
        }

        ~TimelineFacadeImpl() override {
            engine_.removePluginInstanceLifecycleListener(*this);
            for (const auto& [instanceId, listenerId] : plugin_parameter_listener_ids_) {
                if (auto* instance = engine_.getPluginInstance(instanceId)) {
                    if (auto* support = instance->parameterSupport())
                        support->parameterChangeEvent().removeListener(listenerId);
                }
            }
        }

        void notifyTimelineChanged() {
            if (!suppress_timeline_notification_ && timeline_changed_callback_)
                timeline_changed_callback_();
        }

        void emitProjectDocumentEvent(ProjectDocumentEvent event) {
            if (!suppress_project_document_events_)
                project_document_events_.emit(std::move(event));
        }

        static std::string clipObjectId(const TimelineTrack& track, const ClipData* clip, int32_t clipId) {
            if (clip && !clip->referenceId.empty())
                return clip->referenceId;
            return std::format("{}::clip_{:08x}", track.referenceId(), static_cast<uint32_t>(clipId));
        }

        static std::string audioSourceObjectId(const TimelineTrack& track, const ClipData& clip) {
            if (!clip.filepath.empty())
                return "audio-source:" + clip.filepath;
            return "audio-source:" + clipObjectId(track, &clip, clip.clipId);
        }

        size_t audioSourceReferenceCount(const std::string& audioSourceId) const {
            auto countOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> size_t {
                if (!track)
                    return 0;
                size_t count = 0;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clip.clipType == ClipType::Audio && audioSourceObjectId(*track, clip) == audioSourceId)
                        ++count;
                return count;
            };

            size_t count = countOnTrack(master_timeline_track_);
            for (const auto& track : timeline_tracks_)
                count += countOnTrack(track);
            return count;
        }

        int32_t trackIndexFor(const TimelineTrack& track) const {
            if (&track == master_timeline_track_.get())
                return kMasterTrackIndex;
            for (int32_t i = 0; i < static_cast<int32_t>(timeline_tracks_.size()); ++i)
                if (timeline_tracks_[static_cast<size_t>(i)].get() == &track)
                    return i;
            return -1;
        }

        TimelineTrack* findTrackById(const ProjectObjectId& trackId) const {
            if (master_timeline_track_ && master_timeline_track_->referenceId() == trackId)
                return master_timeline_track_.get();
            for (const auto& track : timeline_tracks_)
                if (track && track->referenceId() == trackId)
                    return track.get();
            return nullptr;
        }

        std::optional<std::pair<TimelineTrack*, ClipData>> findClipById(const ProjectObjectId& clipId) const {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<std::pair<TimelineTrack*, ClipData>> {
                if (!track)
                    return std::nullopt;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clipObjectId(*track, &clip, clip.clipId) == clipId)
                        return std::make_pair(track.get(), clip);
                return std::nullopt;
            };

            if (auto found = findOnTrack(master_timeline_track_))
                return found;
            for (const auto& track : timeline_tracks_)
                if (auto found = findOnTrack(track))
                    return found;
            return std::nullopt;
        }

        ProjectClipSnapshot makeClipSnapshot(const TimelineTrack& track, const ClipData& clip) const {
            return ProjectClipSnapshot{
                .clipId = clipObjectId(track, &clip, clip.clipId),
                .trackId = track.referenceId(),
                .trackIndex = trackIndexFor(track),
                .clipNumericId = clip.clipId,
                .sourceNodeId = clip.sourceNodeInstanceId,
                .clipType = clip.clipType,
                .name = clip.name,
                .filepath = clip.filepath,
                .position = clip.position,
                .sampleRate = static_cast<double>(sampleRate_),
                .durationSamples = clip.durationSamples,
                .tickResolution = clip.tickResolution,
                .clipTempo = clip.clipTempo,
                .markers = clip.markers,
                .audioWarps = clip.audioWarps
            };
        }

        void emitClipAdded(TimelineTrack& track, int32_t clipId, int32_t sourceNodeId) {
            auto* clip = track.clipManager().getClip(clipId);
            ProjectDocumentEvent event(ProjectDocumentEventKind::ClipAdded, "clip-added");
            event.setTrackId(track.referenceId())
                .setClipId(clipObjectId(track, clip, clipId))
                .setTrackIndex(trackIndexFor(track))
                .setClipNumericId(clipId)
                .setDetail("source-node-id", static_cast<int64_t>(sourceNodeId));
            if (clip) {
                event.setDetail("clip-type", std::string(clip->clipType == ClipType::Audio ? "audio" : "midi"));
                if (!clip->filepath.empty())
                    event.setDetail("source.file", clip->filepath);
            }
            emitProjectDocumentEvent(std::move(event));

            if (clip && clip->clipType == ClipType::Audio) {
                const auto audioSourceId = audioSourceObjectId(track, *clip);
                if (audioSourceReferenceCount(audioSourceId) == 1) {
                    ProjectDocumentEvent sourceEvent(ProjectDocumentEventKind::AudioSourceAdded, "audio-source-added");
                    sourceEvent.setAudioSourceId(audioSourceId)
                        .setClipId(clipObjectId(track, clip, clipId))
                        .setDetail("source.file", clip->filepath);
                    emitProjectDocumentEvent(std::move(sourceEvent));
                }
            }
        }

        void emitClipRemoved(TimelineTrack& track, const ClipData& clip) {
            ProjectDocumentEvent event(ProjectDocumentEventKind::ClipRemoved, "clip-removed");
            event.setTrackId(track.referenceId())
                .setClipId(clipObjectId(track, &clip, clip.clipId))
                .setTrackIndex(trackIndexFor(track))
                .setClipNumericId(clip.clipId)
                .setDetail("source-node-id", static_cast<int64_t>(clip.sourceNodeInstanceId));
            emitProjectDocumentEvent(std::move(event));

            if (clip.clipType == ClipType::Audio) {
                const auto audioSourceId = audioSourceObjectId(track, clip);
                if (audioSourceReferenceCount(audioSourceId) == 0) {
                    ProjectDocumentEvent sourceEvent(ProjectDocumentEventKind::AudioSourceRemoved, "audio-source-removed");
                    sourceEvent.setAudioSourceId(audioSourceId)
                        .setClipId(clipObjectId(track, &clip, clip.clipId))
                        .setDetail("source.file", clip.filepath);
                    emitProjectDocumentEvent(std::move(sourceEvent));
                }
            }
        }

        void emitClipChanged(TimelineTrack& track, const ClipData& clip, std::string type) {
            ProjectDocumentEvent event(ProjectDocumentEventKind::ClipChanged, std::move(type));
            event.setTrackId(track.referenceId())
                .setClipId(clipObjectId(track, &clip, clip.clipId))
                .setTrackIndex(trackIndexFor(track))
                .setClipNumericId(clip.clipId)
                .setDetail("source-node-id", static_cast<int64_t>(clip.sourceNodeInstanceId))
                .setDetail("clip-type", std::string(clip.clipType == ClipType::Audio ? "audio" : "midi"));
            if (!clip.filepath.empty())
                event.setDetail("source.file", clip.filepath);
            emitProjectDocumentEvent(std::move(event));
        }

        void emitMasterTrackChanged(std::string type = "master-track-changed") {
            ProjectDocumentEvent event(ProjectDocumentEventKind::MasterTrackChanged, std::move(type));
            event.setTrackId(master_timeline_track_ ? master_timeline_track_->referenceId() : "master_track")
                .setTrackIndex(kMasterTrackIndex);
            emitProjectDocumentEvent(std::move(event));
        }

        // ---- TimelineFacade interface ----

        TimelineState& state() override { return timeline_; }

        std::vector<TimelineTrack*> tracks() override {
            std::vector<TimelineTrack*> result;
            result.reserve(timeline_tracks_.size());
            for (auto& t : timeline_tracks_)
                result.push_back(t.get());
            return result;
        }

        TimelineTrack* masterTimelineTrack() override {
            return master_timeline_track_.get();
        }

        int32_t trackIndexForReferenceId(std::string_view trackId) const override {
            return trackIndexForPersistentId(trackId);
        }

        AudioGraphProviderRegistry& audioGraphProviderRegistry() override {
            return audio_graph_provider_registry_;
        }

        const AudioGraphProviderRegistry& audioGraphProviderRegistry() const override {
            return audio_graph_provider_registry_;
        }

        SequencerTrack* resolveSequencerTrack(int32_t trackIndex) {
            if (trackIndex == kMasterTrackIndex)
                return engine_.masterTrack();
            auto& tracks = engine_.tracks();
            if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(tracks.size()))
                return tracks[static_cast<size_t>(trackIndex)];
            return nullptr;
        }

        void captureTrackFragment(int32_t trackIndex, TrackFragmentCallback callback) override {
            if (!callback)
                return;
            // Same constraint as capturing a clip: extensions archive plug-in
            // state here, which ARA forbids while the document is being edited.
            if (project_document_events_.inTransaction()) {
                callback(std::nullopt,
                         "captureTrackFragment must not be called inside a document transaction");
                return;
            }

            auto* timelineTrack = resolveTrack(trackIndex);
            auto* sequencerTrack = resolveSequencerTrack(trackIndex);
            if (!timelineTrack || !sequencerTrack) {
                callback(std::nullopt, "Invalid track index");
                return;
            }

            auto fragment = std::make_shared<ProjectTrackFragment>();
            fragment->referenceId = timelineTrack->referenceId();
            fragment->volume = sequencerTrack->trackGain();
            fragment->muted = sequencerTrack->muted();
            fragment->solo = sequencerTrack->solo();

            // Graph topology, held by value. The .graph.json a project save
            // writes is an artifact of saving; the serialization itself
            // produces bytes.
            auto* provider = audio_graph_provider_registry_.get(sequencerTrack->graph());
            if (!provider) {
                callback(std::nullopt, "The track's graph provider is unavailable");
                return;
            }
            auto graphData = createSerializedProjectGraph(
                *provider,
                sequencerTrack->orderedInstanceIds(),
                sequencerTrack->graph(),
                [this](int32_t instanceId) { return engine_.getPluginInstance(instanceId); },
                nullptr);
            if (!graphData) {
                callback(std::nullopt, "Could not capture the track graph");
                return;
            }
            fragment->graphType = provider->id();
            if (!provider->saveProjectGraph(graphData.get(), fragment->graphBytes)) {
                callback(std::nullopt, "Could not serialize the track graph");
                return;
            }

            // Plugin descriptors now; their state is read asynchronously below.
            auto pluginInstanceIds = std::make_shared<std::vector<int32_t>>();
            for (int32_t instanceId : sequencerTrack->orderedInstanceIds()) {
                auto* instance = engine_.getPluginInstance(instanceId);
                if (!instance)
                    continue;
                ProjectTrackPluginFragment plugin;
                if (auto* node = sequencerTrack->graph().getPluginNode(instanceId))
                    plugin.nodeId = node->nodeId();
                plugin.pluginId = instance->pluginId();
                plugin.format = instance->formatName();
                plugin.displayName = instance->displayName();
                const auto group = sequencerTrack->getInstanceGroup(instanceId);
                plugin.groupIndex = group == 0xFF ? -1 : static_cast<int32_t>(group);
                fragment->plugins.push_back(std::move(plugin));
                pluginInstanceIds->push_back(instanceId);
            }

            for (const auto& clip : timelineTrack->clipManager().getAllClips()) {
                auto clipFragment = captureClipFragment(trackIndex, clip.clipId);
                if (!clipFragment) {
                    callback(std::nullopt,
                             std::format("Failed to capture clip {} of the track", clip.referenceId));
                    return;
                }
                fragment->clips.push_back(std::move(*clipFragment));
            }

            for (auto* extension : projectSerializationExtensionsSnapshot()) {
                std::vector<uint8_t> state;
                std::string extensionError;
                if (!extension->captureTrackFragmentState(fragment->referenceId, state, extensionError)) {
                    callback(std::nullopt,
                             std::format("Extension {} could not capture its state for track {}: {}",
                                         extension->extensionId(), fragment->referenceId, extensionError));
                    return;
                }
                if (!state.empty())
                    fragment->extensionState[std::string(extension->extensionId())] = std::move(state);
            }

            // Plugin state is callback-based, so the remaining work is a chain
            // rather than a loop. This is the reason capture cannot simply
            // return a fragment the way the clip version does.
            auto sharedCallback = std::make_shared<TrackFragmentCallback>(std::move(callback));
            auto step = std::make_shared<std::function<void(size_t)>>();
            *step = [this, fragment, pluginInstanceIds, sharedCallback, step](size_t index) {
                if (index >= pluginInstanceIds->size()) {
                    (*sharedCallback)(std::move(*fragment), std::string{});
                    return;
                }
                auto* instance = engine_.getPluginInstance((*pluginInstanceIds)[index]);
                if (!instance) {
                    (*step)(index + 1);
                    return;
                }
                instance->requestState(
                    StateContextType::Project, false, nullptr,
                    [fragment, sharedCallback, step, index](
                        std::vector<uint8_t> state, std::string error, void*) mutable {
                        if (!error.empty()) {
                            (*sharedCallback)(std::nullopt, std::move(error));
                            return;
                        }
                        fragment->plugins[index].state = std::move(state);
                        (*step)(index + 1);
                    });
            };
            (*step)(0);
        }

        void attachTrackFragment(
            const ProjectTrackFragment& fragment,
            ProjectTrackAttachOptions options,
            TrackAttachCallback callback) override {
            if (!callback)
                return;
            // Copied because the chain below outlives this call.
            auto source = std::make_shared<ProjectTrackFragment>(fragment);
            auto sharedCallback = std::make_shared<TrackAttachCallback>(std::move(callback));

            if (options.idPolicy == ProjectObjectIdPolicy::Restore)
                pending_track_reference_id_ = source->referenceId;
            const int32_t trackIndex = engine_.addEmptyTrack(options.insertionIndex);
            pending_track_reference_id_.clear();
            if (trackIndex < 0) {
                (*sharedCallback)(-1, "Failed to create track");
                return;
            }

            auto completed = std::make_shared<bool>(false);
            auto fail = std::make_shared<std::function<void(std::string)>>();
            *fail = [this, trackIndex, sharedCallback, completed](std::string error) {
                if (*completed)
                    return;
                *completed = true;
                if (!engine_.removeTrack(trackIndex))
                    error += " The partially attached track could not be removed.";
                (*sharedCallback)(-1, std::move(error));
            };

            if (auto* sequencerTrack = resolveSequencerTrack(trackIndex)) {
                sequencerTrack->trackGain(source->volume);
                sequencerTrack->muted(source->muted);
                sequencerTrack->solo(source->solo);
            } else {
                (*fail)("The created sequencer track is unavailable");
                return;
            }
            if (options.includePlugins
                && !source->graphType.empty()
                && !replaceTrackGraphType(
                    trackIndex, source->graphType, engine_.umpBufferSizeInBytes(),
                    ProjectMutationOrigin::Internal)) {
                (*fail)(std::format(
                    "Could not create graph type {} while attaching the track",
                    source->graphType));
                return;
            }

            // Applied once every plugin has been created and, if requested,
            // had its state restored. Clips and extension state are synchronous
            // and share one transaction so observers see the finished track
            // rather than it filling in.
            auto finish = [this, source, options, trackIndex, sharedCallback, completed, fail]() {
                std::string error;
                {
                    ProjectDocumentTransaction transaction(project_document_events_);
                    if (options.includeClips) {
                        for (const auto& clipFragment : source->clips) {
                            auto result = attachClipFragment(
                                trackIndex, clipFragment, options.idPolicy);
                            if (!result.success) {
                                error = result.error.empty()
                                    ? "Failed to restore a clip while attaching the track"
                                    : std::move(result.error);
                                break;
                            }
                        }
                    }

                    if (error.empty()) {
                        auto* timelineTrack = resolveTrack(trackIndex);
                        if (!timelineTrack) {
                            error = "The created timeline track is unavailable";
                        } else {
                            for (auto* extension : projectSerializationExtensionsSnapshot()) {
                                const auto it = source->extensionState.find(std::string(extension->extensionId()));
                                static const std::vector<uint8_t> kNoState{};
                                const auto& state = it == source->extensionState.end() ? kNoState : it->second;
                                std::string extensionError;
                                if (!extension->restoreTrackFragmentState(
                                        timelineTrack->referenceId(), state, extensionError)) {
                                    error = std::format(
                                        "Extension {} failed to restore state for track {}: {}",
                                        extension->extensionId(),
                                        timelineTrack->referenceId(),
                                        extensionError);
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!error.empty()) {
                    (*fail)(std::move(error));
                    return;
                }
                if (*completed)
                    return;
                *completed = true;
                (*sharedCallback)(trackIndex, std::string{});
            };

            auto finishHolder = std::make_shared<std::function<void()>>(
                [this, source, options, trackIndex, finish = std::move(finish), fail]() mutable {
                    if (options.includePlugins && !source->graphBytes.empty()) {
                        auto* provider = audio_graph_provider_registry_.get(source->graphType);
                        auto* sequencerTrack = resolveSequencerTrack(trackIndex);
                        auto metadata = UapmdProjectPluginGraphData::create();
                        if (!provider || !sequencerTrack || !metadata) {
                            (*fail)("Could not resolve the captured track graph");
                            return;
                        }
                        metadata->graphType(source->graphType);
                        auto graphData = loadSerializedProjectGraph(
                            *provider, *metadata, source->graphBytes);
                        if (!graphData || !provider->deserializeRuntimeGraph(
                                graphData.get(),
                                sequencerTrack->graph(),
                                sequencerTrack->orderedInstanceIds())) {
                            (*fail)("Could not restore the captured track graph topology");
                            return;
                        }
                    }
                    finish();
                });
            if (!options.includePlugins) {
                (*finishHolder)();
                return;
            }
            if (source->plugins.empty()) {
                (*finishHolder)();
                return;
            }

            // Instantiating a plugin is callback-based, so plugins are added
            // one at a time rather than in a loop.
            auto step = std::make_shared<std::function<void(size_t)>>();
            *step = [this, source, options, trackIndex, finishHolder, step, fail](size_t index) {
                if (index >= source->plugins.size()) {
                    (*finishHolder)();
                    return;
                }
                auto& plugin = source->plugins[index];
                // Restore reuses the captured node identity so that anything
                // keyed by it reconnects; Mint leaves it empty and a fresh one
                // is derived from the new instance.
                auto restoreNodeId = options.idPolicy == ProjectObjectIdPolicy::Restore
                    ? plugin.nodeId
                    : std::string{};
                // addPluginToTrack takes non-const references.
                auto format = plugin.format;
                auto pluginId = plugin.pluginId;
                engine_.addPluginToTrack(
                    trackIndex, format, pluginId,
                    [this, source, options, step, index, fail](int32_t instanceId, int32_t, std::string error) {
                        auto& added = source->plugins[index];
                        if (!error.empty() || instanceId < 0) {
                            (*fail)(std::format(
                                "Failed to instantiate {} while attaching the track: {}",
                                added.displayName.empty() ? added.pluginId : added.displayName,
                                error));
                            return;
                        }
                        if (added.groupIndex >= 0 && added.groupIndex <= 15
                            && !engine_.setInstanceGroup(
                                instanceId, static_cast<uint8_t>(added.groupIndex))) {
                            (*fail)(std::format(
                                "Failed to restore the MIDI group for {}",
                                added.displayName.empty() ? added.pluginId : added.displayName));
                            return;
                        }

                        auto* instance = engine_.getPluginInstance(instanceId);
                        if (!instance) {
                            (*fail)(std::format(
                                "The restored plugin instance for {} is unavailable",
                                added.displayName.empty() ? added.pluginId : added.displayName));
                            return;
                        }
                        if (!options.includePluginState || added.state.empty()) {
                            (*step)(index + 1);
                            return;
                        }
                        instance->loadState(
                            added.state, StateContextType::Project, false, nullptr,
                            [step, index, fail,
                             displayName = added.displayName,
                             pluginId = added.pluginId](std::string loadError, void*) {
                                if (!loadError.empty()) {
                                    (*fail)(std::format(
                                        "Failed to restore state for {} while attaching the track: {}",
                                        displayName.empty() ? pluginId : displayName,
                                        loadError));
                                    return;
                                }
                                (*step)(index + 1);
                            });
                    },
                    std::move(restoreNodeId));
            };
            (*step)(0);
        }

        void addEmptyTrack(
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) override {
            if (!callback)
                return;
            const bool recordsHistory = origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
            const auto undoState = undo_engine_.state();
            if (recordsHistory && undoState.busy && !undoState.compoundOpen) {
                callback(-1, "An undo history operation is already pending");
                return;
            }

            const auto trackIndex = engine_.addEmptyTrack();
            if (trackIndex < 0) {
                callback(-1, "Failed to create track");
                return;
            }
            if (!recordsHistory) {
                callback(trackIndex, {});
                return;
            }

            captureTrackFragment(
                trackIndex,
                [this, trackIndex, origin, callback = std::move(callback)](
                    std::optional<ProjectTrackFragment> fragment,
                    std::string error) mutable {
                    auto finish = [this, trackIndex, origin,
                                   callback = std::move(callback),
                                   fragment = std::move(fragment),
                                   error = std::move(error)]() mutable {
                        if (!fragment) {
                            engine_.removeTrack(trackIndex);
                            callback(
                                -1,
                                error.empty()
                                    ? "Could not capture the added track for undo history"
                                    : std::move(error));
                            return;
                        }
                        auto operation = makeTrackStructureOperation(
                            TrackStructureUndoOperation::InitialDirection::Addition,
                            trackIndex,
                            std::move(*fragment));
                        undo_engine_.recordPerformed(
                            std::move(operation),
                            origin,
                            [this, trackIndex, callback = std::move(callback)](
                                ProjectUndoResult result) mutable {
                                if (!result.succeeded()) {
                                    engine_.removeTrack(trackIndex);
                                    callback(-1, std::move(result.error));
                                    return;
                                }
                                callback(trackIndex, {});
                            });
                    };
                    dispatchToModelThread(std::move(finish));
                });
        }

        void removeTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin,
            TrackAttachCallback callback) override {
            if (!callback)
                return;
            const bool recordsHistory = origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
            auto* track = resolveTrack(trackIndex);
            if (!track || trackIndex == kMasterTrackIndex) {
                callback(-1, "Invalid track index");
                return;
            }
            if (!recordsHistory) {
                if (engine_.removeTrack(trackIndex))
                    callback(trackIndex, {});
                else
                    callback(-1, "Failed to remove track");
                return;
            }
            const auto undoState = undo_engine_.state();
            if (undoState.busy && !undoState.compoundOpen) {
                callback(-1, "An undo history operation is already pending");
                return;
            }

            const auto expectedReferenceId = track->referenceId();
            captureTrackFragment(
                trackIndex,
                [this, trackIndex, origin, expectedReferenceId,
                 callback = std::move(callback)](
                    std::optional<ProjectTrackFragment> fragment,
                    std::string error) mutable {
                    auto finish = [this, trackIndex, origin, expectedReferenceId,
                                   callback = std::move(callback),
                                   fragment = std::move(fragment),
                                   error = std::move(error)]() mutable {
                        if (!fragment) {
                            callback(
                                -1,
                                error.empty() ? "Could not capture the track" : std::move(error));
                            return;
                        }
                        if (fragment->referenceId != expectedReferenceId
                            || trackIndexForPersistentId(expectedReferenceId) != trackIndex) {
                            callback(-1, "The track changed position while it was being captured");
                            return;
                        }

                        auto operation = makeTrackStructureOperation(
                            TrackStructureUndoOperation::InitialDirection::Removal,
                            trackIndex,
                            std::move(*fragment));
                        undo_engine_.perform(
                            std::move(operation),
                            origin,
                            [trackIndex, callback = std::move(callback)](
                                ProjectUndoResult result) mutable {
                                if (!result.succeeded()) {
                                    callback(-1, std::move(result.error));
                                    return;
                                }
                                callback(trackIndex, {});
                            });
                    };
                    dispatchToModelThread(std::move(finish));
                });
        }

        void beginDocumentTransaction() override {
            project_document_events_.beginTransaction();
        }

        void endDocumentTransaction() override {
            project_document_events_.endTransaction();
        }

        ProjectDocumentEventSource& projectDocumentEvents() override {
            return project_document_events_;
        }

        ProjectUndoEngine& undoEngine() override {
            return undo_engine_;
        }

        ProjectDocumentView& projectDocumentView() override {
            return *this;
        }

        AudioSourceRepository& audioSourceRepository() override {
            return *audio_source_repository_;
        }

        void setAudioSourceRepository(std::shared_ptr<AudioSourceRepository> repository) override {
            if (repository)
                audio_source_repository_ = std::move(repository);
            else
                audio_source_repository_ = std::make_shared<FileAudioSourceRepository>();
        }

        ProjectRevision currentRevision() const override {
            return project_document_events_.currentRevision();
        }

        std::optional<ProjectObjectId> masterTrackId() const override {
            if (!master_timeline_track_)
                return std::nullopt;
            return master_timeline_track_->referenceId();
        }

        std::vector<ProjectObjectId> trackIds() const override {
            std::vector<ProjectObjectId> result;
            result.reserve(timeline_tracks_.size());
            for (const auto& track : timeline_tracks_)
                if (track)
                    result.push_back(track->referenceId());
            return result;
        }

        std::vector<ProjectObjectId> clipIds(ProjectObjectId trackId) const override {
            std::vector<ProjectObjectId> result;
            auto* track = findTrackById(trackId);
            if (!track)
                return result;
            auto clips = track->clipManager().getAllClips();
            result.reserve(clips.size());
            for (const auto& clip : clips)
                result.push_back(clipObjectId(*track, &clip, clip.clipId));
            return result;
        }

        std::vector<ProjectObjectId> audioSourceIds() const override {
            std::vector<ProjectObjectId> result;
            auto collect = [&result](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return;
                for (const auto& clip : track->clipManager().getAllClips())
                    if (clip.clipType == ClipType::Audio)
                        result.push_back(audioSourceObjectId(*track, clip));
            };
            collect(master_timeline_track_);
            for (const auto& track : timeline_tracks_)
                collect(track);
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        std::optional<ProjectTrackSnapshot> getTrack(ProjectObjectId trackId) const override {
            auto* track = findTrackById(trackId);
            if (!track)
                return std::nullopt;
            return ProjectTrackSnapshot{
                .trackId = track->referenceId(),
                .trackIndex = trackIndexFor(*track),
                .masterTrack = track == master_timeline_track_.get()
            };
        }

        std::optional<ProjectClipSnapshot> getClip(ProjectObjectId clipId) const override {
            auto found = findClipById(clipId);
            if (!found)
                return std::nullopt;
            return makeClipSnapshot(*found->first, found->second);
        }

        std::optional<ProjectAudioSourceSnapshot> getAudioSource(ProjectObjectId audioSourceId) const override {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) -> std::optional<ProjectAudioSourceSnapshot> {
                if (!track)
                    return std::nullopt;
                for (const auto& clip : track->clipManager().getAllClips()) {
                    if (clip.clipType != ClipType::Audio)
                        continue;
                    if (audioSourceObjectId(*track, clip) != audioSourceId)
                        continue;
                    ProjectAudioSourceSnapshot snapshot;
                    snapshot.audioSourceId = audioSourceId;
                    snapshot.clipId = clipObjectId(*track, &clip, clip.clipId);
                    snapshot.filepath = clip.filepath;
                    snapshot.sourceNodeId = clip.sourceNodeInstanceId;
                    if (auto info = audio_source_repository_->getAudioSourceInfo(audioSourceId, clip.filepath)) {
                        snapshot.channelCount = info->channelCount;
                        snapshot.sampleRate = info->sampleRate;
                        snapshot.frameCount = info->frameCount;
                    } else if (clip.filepath.empty()) {
                        snapshot.channelCount = std::max<uint32_t>(1, track->channelCount());
                        snapshot.sampleRate = static_cast<double>(sampleRate_);
                        snapshot.frameCount = std::max<int64_t>(1, clip.durationSamples);
                    }
                    return snapshot;
                }
                return std::nullopt;
            };

            if (auto found = findOnTrack(master_timeline_track_))
                return found;
            for (const auto& track : timeline_tracks_)
                if (auto found = findOnTrack(track))
                    return found;
            return std::nullopt;
        }

        bool readClipUmpContent(
            ProjectObjectId clipId,
            std::vector<uapmd_ump_t>& events,
            std::vector<uint64_t>& timestampsInTicks,
            uint32_t& tickResolution) const override {
            events.clear();
            timestampsInTicks.clear();
            tickResolution = 0;

            auto found = findClipById(clipId);
            if (!found || found->second.clipType != ClipType::Midi)
                return false;

            auto sourceNode = found->first->getSourceNode(found->second.sourceNodeInstanceId);
            auto* midiSource = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
            if (!midiSource)
                return false;

            events = midiSource->umpEvents();
            timestampsInTicks = midiSource->eventTimestampsTicks();
            tickResolution = midiSource->tickResolution();
            return true;
        }

        bool readAudioSourceSamples(
            ProjectObjectId audioSourceId,
            int64_t startFrame,
            int64_t frameCount,
            float** destination,
            uint32_t destinationChannels) const override {
            auto findOnTrack = [&](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return false;
                for (const auto& clip : track->clipManager().getAllClips()) {
                    if (clip.clipType != ClipType::Audio)
                        continue;
                    if (audioSourceObjectId(*track, clip) != audioSourceId)
                        continue;
                    if (clip.filepath.empty()) {
                        if (!destination || startFrame < 0 || frameCount < 0)
                            return false;
                        for (uint32_t ch = 0; ch < destinationChannels; ++ch)
                            if (destination[ch])
                                std::memset(destination[ch], 0, static_cast<size_t>(frameCount) * sizeof(float));
                        return true;
                    }
                    return audio_source_repository_->readAudioSourceSamples(
                        audioSourceId,
                        clip.filepath,
                        startFrame,
                        frameCount,
                        destination,
                        destinationChannels);
                }
                return false;
            };

            if (findOnTrack(master_timeline_track_))
                return true;
            for (const auto& track : timeline_tracks_)
                if (findOnTrack(track))
                    return true;
            return false;
        }

        std::optional<TrackGraphSnapshot> captureTrackGraphSnapshot(
            int32_t trackIndex) {
            auto* track = resolveSequencerTrack(trackIndex);
            if (!track)
                return std::nullopt;
            auto* provider = audio_graph_provider_registry_.get(track->graph());
            if (!provider)
                return std::nullopt;
            auto graphData = createSerializedProjectGraph(
                *provider,
                track->orderedInstanceIds(),
                track->graph(),
                [this](int32_t instanceId) {
                    return engine_.getPluginInstance(instanceId);
                },
                nullptr);
            if (!graphData)
                return std::nullopt;
            TrackGraphSnapshot snapshot;
            snapshot.graphType = provider->id();
            if (!provider->saveProjectGraph(
                    graphData.get(), snapshot.graphBytes))
                return std::nullopt;
            return snapshot;
        }

        bool applyTrackGraphSnapshot(
            std::string_view trackReferenceId,
            const TrackGraphSnapshot& snapshot,
            size_t eventBufferSizeInBytes) {
            auto* provider =
                audio_graph_provider_registry_.get(snapshot.graphType);
            const auto trackIndex =
                trackIndexForPersistentId(trackReferenceId);
            if (!provider
                || (trackIndex < 0 && trackIndex != kMasterTrackIndex))
                return false;
            auto newGraph = provider->createGraph(eventBufferSizeInBytes);
            if (!newGraph
                || !engine_.replaceTrackGraph(trackIndex, std::move(newGraph)))
                return false;
            if (snapshot.graphBytes.empty())
                return true;

            auto* track = resolveSequencerTrack(trackIndex);
            auto metadata = UapmdProjectPluginGraphData::create();
            if (!track || !metadata)
                return false;
            metadata->graphType(snapshot.graphType);
            auto graphData = loadSerializedProjectGraph(
                *provider,
                *metadata,
                snapshot.graphBytes);
            if (!graphData
                || !provider->deserializeRuntimeGraph(
                    graphData.get(),
                    track->graph(),
                    track->orderedInstanceIds()))
                return false;
            onTrackGraphChanged(trackIndex);
            return true;
        }

        bool replaceTrackGraphType(
            int32_t trackIndex,
            const std::string& graphTypeId,
            size_t eventBufferSizeInBytes,
            ProjectMutationOrigin origin) override {
            auto* provider = audio_graph_provider_registry_.get(graphTypeId);
            if (!provider)
                return false;

            SequencerTrack* track = trackIndex == kMasterTrackIndex
                ? engine_.masterTrack()
                : (trackIndex >= 0 && trackIndex < static_cast<int32_t>(engine_.tracks().size())
                    ? engine_.tracks()[static_cast<size_t>(trackIndex)]
                    : nullptr);
            if (!track)
                return false;

            if (track->graph().providerId() == provider->id())
                return true;
            auto* timelineTrack = resolveTrack(trackIndex);
            if (!timelineTrack)
                return false;
            auto before = captureTrackGraphSnapshot(trackIndex);
            if (!before)
                return false;
            auto trackReferenceId = timelineTrack->referenceId();
            TrackGraphSnapshot requested;
            requested.graphType = graphTypeId;
            if (!applyTrackGraphSnapshot(
                    trackReferenceId,
                    requested,
                    eventBufferSizeInBytes))
                return false;
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return true;

            auto after = captureTrackGraphSnapshot(trackIndex);
            if (!after) {
                applyTrackGraphSnapshot(
                    trackReferenceId, *before, eventBufferSizeInBytes);
                return false;
            }
            auto apply = [this, eventBufferSizeInBytes](
                             std::string_view persistentTrackId,
                             const TrackGraphSnapshot& desired,
                             const TrackGraphSnapshot& compensation) {
                if (applyTrackGraphSnapshot(
                        persistentTrackId,
                        desired,
                        eventBufferSizeInBytes))
                    return true;
                applyTrackGraphSnapshot(
                    persistentTrackId,
                    compensation,
                    eventBufferSizeInBytes);
                return false;
            };
            auto operation = std::make_shared<TrackGraphUndoOperation>(
                trackReferenceId,
                *before,
                *after,
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.recordPerformed(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            if (result->has_value() && result->value().succeeded())
                return true;
            applyTrackGraphSnapshot(
                trackReferenceId, *before, eventBufferSizeInBytes);
            return false;
        }

        bool materializeProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            size_t eventBufferSizeInBytes) override {
            if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                return true;

            auto* provider = audio_graph_provider_registry_.get(projectTrack->graph()->graphType());
            if (!provider)
                return false;

            int32_t trackIndex = -1;
            if (engine_.masterTrack() == sequencerTrack)
                trackIndex = kMasterTrackIndex;
            else {
                auto tracks = engine_.tracks();
                for (int32_t i = 0; i < static_cast<int32_t>(tracks.size()); ++i) {
                    if (tracks[static_cast<size_t>(i)] == sequencerTrack) {
                        trackIndex = i;
                        break;
                    }
                }
            }

            if (trackIndex == -1)
                return false;
            if (!replaceTrackGraphType(
                    trackIndex,
                    provider->id(),
                    eventBufferSizeInBytes,
                    ProjectMutationOrigin::Internal))
                return false;
            return provider->deserializeRuntimeGraph(
                projectTrack->graph(), sequencerTrack->graph(), sequencerTrack->orderedInstanceIds());
        }

        bool saveProjectGraph(
            UapmdProjectTrackData* projectTrack,
            SequencerTrack* sequencerTrack,
            const std::filesystem::path& projectDir,
            const std::filesystem::path& graphDir,
            const std::string& scopeLabel,
            std::string& error) override {
            if (!projectTrack || !sequencerTrack)
                return true;

            auto* provider = audio_graph_provider_registry_.get(sequencerTrack->graph());
            if (!provider)
                return false;

            auto graphFilename = std::format(
                "{}.graph.json",
                urlEscapeFilenameComponent(scopeLabel));
            auto graphPath = graphDir / graphFilename;
            auto recordedPath = graphPath;
            if (!projectDir.empty())
                recordedPath = makeRelativePath(projectDir, graphPath);

            std::vector<uint8_t> graphBytes;
            if (!provider->saveProjectGraph(projectTrack->graph(), graphBytes)) {
                error = std::format("Failed to serialize graph {}", graphPath.string());
                return false;
            }

            if (!writeBinaryFile(graphPath, graphBytes, error))
                return false;

            auto graph = UapmdProjectPluginGraphData::create();
            graph->graphType(provider->id());
            graph->externalFile(recordedPath);
            projectTrack->graph(std::move(graph));
            return true;
        }

        void addProjectSerializationExtension(ProjectSerializationExtension& extension) override {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            if (std::find(project_serialization_extensions_.begin(), project_serialization_extensions_.end(), &extension) ==
                project_serialization_extensions_.end())
                project_serialization_extensions_.push_back(&extension);
        }

        void removeProjectSerializationExtension(ProjectSerializationExtension& extension) override {
            std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
            std::erase(project_serialization_extensions_, &extension);
        }

        bool saveProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) override {
            std::vector<ProjectSerializationExtension*> extensions;
            {
                std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
                extensions = project_serialization_extensions_;
            }
            for (auto* extension : extensions) {
                if (!extension)
                    continue;
                std::string extensionError;
                if (!extension->saveProjectData(project, extensionError)) {
                    error = std::format("Failed to save project data for extension {}: {}",
                                        extension->extensionId(), extensionError);
                    return false;
                }
            }
            return true;
        }

        bool loadProjectDataExtensions(
            UapmdProjectData& project,
            std::string& error) override {
            std::vector<ProjectSerializationExtension*> extensions;
            {
                std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
                extensions = project_serialization_extensions_;
            }
            for (auto* extension : extensions) {
                if (!extension)
                    continue;
                std::string extensionError;
                if (!extension->loadProjectData(project, extensionError)) {
                    error = std::format("Failed to load project data for extension {}: {}",
                                        extension->extensionId(), extensionError);
                    return false;
                }
            }
            return true;
        }

        bool saveProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) override {
            std::vector<ProjectSerializationExtension*> extensions;
            {
                std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
                extensions = project_serialization_extensions_;
            }

            sequencer_detail::FilesystemProjectSerializationWriteContext context(projectFile, projectDir);
            for (auto* extension : extensions) {
                if (!extension)
                    continue;
                std::string extensionError;
                if (!extension->saveProjectExtensionData(context, extensionError)) {
                    error = std::format("Failed to save project extension {}: {}",
                                        extension->extensionId(),
                                        extensionError);
                    return false;
                }
            }
            return true;
        }

        bool loadProjectExtensionData(
            const std::filesystem::path& projectFile,
            const std::filesystem::path& projectDir,
            std::string& error) override {
            std::vector<ProjectSerializationExtension*> extensions;
            {
                std::lock_guard<std::mutex> lock(project_serialization_extensions_mutex_);
                extensions = project_serialization_extensions_;
            }

            sequencer_detail::FilesystemProjectSerializationReadContext context(projectFile, projectDir);
            for (auto* extension : extensions) {
                if (!extension)
                    continue;
                std::string extensionError;
                if (!extension->loadProjectExtensionData(context, extensionError)) {
                    error = std::format("Failed to load project extension {}: {}",
                                        extension->extensionId(),
                                        extensionError);
                    return false;
                }
            }
            return true;
        }

        void queueProjectGraphSerialization(
            PendingProjectSaveContext& operation,
            SequencerTrack* sequencerTrack,
            UapmdProjectTrackData& projectTrack,
            const std::string& scopeLabel) {
            const auto* graphProvider = sequencerTrack
                ? audio_graph_provider_registry_.get(sequencerTrack->graph())
                : audio_graph_provider_registry_.get("");
            if (!sequencerTrack || !graphProvider)
                return;

            auto graphData = createSerializedProjectGraph(
                *graphProvider,
                sequencerTrack->orderedInstanceIds(),
                sequencerTrack->graph(),
                [this](int32_t instanceId) {
                    return engine_.getPluginInstance(instanceId);
                },
                [&operation, &scopeLabel](int32_t instanceId, size_t pluginOrder, AudioPluginInstanceAPI* instance,
                                          const std::function<void(const std::string& relativePath)>& setStateFile) {
                    if (setStateFile)
                        setStateFile({});
                    operation.pending_states.push_back(PendingProjectPluginState{
                        .instance_id = instanceId,
                        .plugin_order = pluginOrder,
                        .instance = instance,
                        .set_state_file = setStateFile,
                        .scope_label = scopeLabel
                    });
                });
            if (!graphData)
                return;

            projectTrack.graph(std::move(graphData));
            operation.pending_graphs.push_back(PendingProjectGraphSave{
                .track = &projectTrack,
                .sequencer_track = sequencerTrack,
                .scope_label = scopeLabel
            });
        }

        void saveProject(
            const std::filesystem::path& projectFile,
            ProjectSaveOptions options,
            ProjectSaveCallback callback) override {
            auto operation = std::make_shared<PendingProjectSaveContext>();
            operation->project_file = projectFile;
            operation->emit_document_event = options.emitDocumentEvent;
            operation->history_state_id = undo_engine_.state().currentStateId;
            operation->callback = std::move(callback);

            auto complete = [operation](ProjectResult result) mutable {
                if (!operation->callback)
                    return;
                auto callback = std::move(operation->callback);
                callback(std::move(result));
            };

            if (engine_.frozenTrackManager().hasBusyTrack()) {
                complete(ProjectResult{
                    false,
                    "Unfreeze the busy track before saving the project"});
                return;
            }
            if (undo_engine_.state().busy) {
                complete(ProjectResult{
                    false,
                    "Wait for the pending undo operation before saving the project"});
                return;
            }
            if (projectFile.empty()) {
                complete(ProjectResult{false, "Project path is empty"});
                return;
            }

            try {
                operation->project_dir = projectFile.parent_path();
                if (!operation->project_dir.empty())
                    std::filesystem::create_directories(operation->project_dir);
                auto clipDir = operation->project_dir / "clips";
                operation->plugin_state_dir = operation->project_dir / "plugin_states";
                operation->graph_dir = operation->project_dir / "graphs";
                operation->project = UapmdProjectData::create();
                std::unordered_set<int32_t> excludedTrackIndexes(
                    options.excludedTrackIndexes.begin(),
                    options.excludedTrackIndexes.end());

                struct SerializedTrackClips {
                    int32_t trackIndex{0};
                    std::vector<ClipData> clips;
                };
                std::unordered_map<std::string, UapmdProjectClipData*> serializedClipLookup;
                std::vector<SerializedTrackClips> serializedTracks;
                size_t midiExportCounter = 0;

                auto sequencerTracks = engine_.tracks();
                auto timelineTracks = tracks();
                for (size_t trackIndex = 0; trackIndex < timelineTracks.size(); ++trackIndex) {
                    if (excludedTrackIndexes.contains(static_cast<int32_t>(trackIndex)))
                        continue;

                    auto* timelineTrack = timelineTracks[trackIndex];
                    if (!timelineTrack)
                        continue;

                    auto projectTrack = UapmdProjectTrackData::create();
                    projectTrack->referenceId(timelineTrack->referenceId());
                    auto clips = sequencer_detail::sortedTrackClips(*timelineTrack);
                    serializedTracks.push_back(SerializedTrackClips{
                        static_cast<int32_t>(trackIndex),
                        clips});

                    for (const auto& clip : clips) {
                        std::string clipError;
                        if (!sequencer_detail::serializeProjectClip(
                                *timelineTrack,
                                clip,
                                *projectTrack,
                                serializedClipLookup,
                                clipDir,
                                operation->project_dir,
                                std::format("track{}_clip_", trackIndex),
                                std::format("track{}_", trackIndex),
                                std::format("Clip {} on track {}", clip.clipId, trackIndex),
                                false,
                                midiExportCounter,
                                clipError)) {
                            complete(ProjectResult{false, std::move(clipError)});
                            return;
                        }
                    }

                    SequencerTrack* sequencerTrack = trackIndex < sequencerTracks.size()
                        ? sequencerTracks[trackIndex]
                        : nullptr;
                    if (sequencerTrack)
                        projectTrack->volume(sequencerTrack->trackGain());
                    if (sequencerTrack) {
                        projectTrack->muted(sequencerTrack->muted());
                        projectTrack->solo(sequencerTrack->solo());
                    }
                    bool hasClips = !projectTrack->clips().empty();
                    bool hasPlugins = sequencerTrack && !sequencerTrack->orderedInstanceIds().empty();
                    bool hasMixerState = sequencerTrack &&
                        (sequencerTrack->trackGain() != 1.0 || sequencerTrack->muted() || sequencerTrack->solo());
                    if (!hasClips && !hasPlugins && !hasMixerState)
                        continue;

                    queueProjectGraphSerialization(
                        *operation,
                        sequencerTrack,
                        *projectTrack,
                        std::format("track{}", trackIndex));

                    operation->project->addTrack(std::move(projectTrack));
                }

                if (auto* masterTrack = operation->project->masterTrack()) {
                    masterTrack->clips().clear();
                    masterTrack->markers(std::move(options.masterTrackMarkers));
                    if (engine_.masterTrack())
                        masterTrack->volume(engine_.masterTrack()->trackGain());
                    if (master_timeline_track_) {
                        auto clips = sequencer_detail::sortedTrackClips(*master_timeline_track_);
                        serializedTracks.push_back(SerializedTrackClips{
                            kMasterTrackIndex,
                            clips});

                        for (const auto& clip : clips) {
                            if (clip.clipType != ClipType::Midi)
                                continue;

                            std::string clipError;
                            if (!sequencer_detail::serializeProjectClip(
                                    *master_timeline_track_,
                                    clip,
                                    *masterTrack,
                                    serializedClipLookup,
                                    clipDir,
                                    operation->project_dir,
                                    "master_clip_",
                                    "",
                                    "Master clip",
                                    true,
                                    midiExportCounter,
                                    clipError)) {
                                complete(ProjectResult{false, std::move(clipError)});
                                return;
                            }
                        }
                    }

                    queueProjectGraphSerialization(
                        *operation,
                        engine_.masterTrack(),
                        *masterTrack,
                        "master");
                }

                for (const auto& serializedTrack : serializedTracks) {
                    for (const auto& clip : serializedTrack.clips) {
                        auto clipIt = serializedClipLookup.find(clip.referenceId);
                        if (clipIt == serializedClipLookup.end())
                            continue;

                        const auto timeReference = clip.timeReference(sampleRate_);
                        UapmdTimelinePosition pos{};
                        if (!timeReference.referenceId.empty()) {
                            auto anchorIt = serializedClipLookup.find(timeReference.referenceId);
                            if (anchorIt != serializedClipLookup.end())
                                pos.anchor = anchorIt->second;
                        }
                        pos.origin = (timeReference.type == TimeReferenceType::ContainerEnd)
                            ? UapmdAnchorOrigin::End
                            : UapmdAnchorOrigin::Start;
                        pos.samples = static_cast<uint64_t>(std::max<int64_t>(0,
                            TimelinePosition::fromSeconds(timeReference.offset, sampleRate_).samples));
                        clipIt->second->position(pos);
                    }
                }
            } catch (const std::exception& e) {
                complete(ProjectResult{false, e.what()});
                return;
            }

            auto runNext = std::make_shared<std::function<void()>>();
            *runNext = [this, operation, complete, runNext]() mutable {
                if (operation->next_pending_state >= operation->pending_states.size()) {
                    for (const auto& pendingGraph : operation->pending_graphs) {
                        if (!pendingGraph.track || !pendingGraph.sequencer_track || !pendingGraph.track->graph())
                            continue;

                        std::string graphWriteError;
                        if (!saveProjectGraph(
                                pendingGraph.track,
                                pendingGraph.sequencer_track,
                                operation->project_dir,
                                operation->graph_dir,
                                pendingGraph.scope_label,
                                graphWriteError)) {
                            complete(ProjectResult{false, std::move(graphWriteError)});
                            return;
                        }
                    }
                    std::string projectDataError;
                    if (!saveProjectDataExtensions(*operation->project, projectDataError)) {
                        complete(ProjectResult{false, std::move(projectDataError)});
                        return;
                    }
                    if (!UapmdProjectDataWriter::write(operation->project.get(), operation->project_file)) {
                        complete(ProjectResult{false, "Failed to write project file"});
                        return;
                    }
                    std::string extensionError;
                    if (!saveProjectExtensionData(
                            operation->project_file,
                            operation->project_dir,
                            extensionError)) {
                        complete(ProjectResult{false, std::move(extensionError)});
                        return;
                    }
                    auto finishSuccessfulSave = [this, operation, complete]() mutable {
                        if (operation->emit_document_event) {
                            undo_engine_.markStateSaved(operation->history_state_id);
                            ProjectDocumentEvent savedEvent(ProjectDocumentEventKind::ProjectSaved, "project-saved");
                            savedEvent.setProjectId(operation->project_file.string())
                                .setDetail("source.file", operation->project_file.string());
                            emitProjectDocumentEvent(std::move(savedEvent));
                        }
                        complete(ProjectResult{true, {}});
                    };
                    if (remidy::EventLoop::runningOnMainThread())
                        finishSuccessfulSave();
                    else
                        remidy::EventLoop::enqueueTaskOnMainThread(std::move(finishSuccessfulSave));
                    return;
                }

                auto pending = operation->pending_states[operation->next_pending_state];
                if (!pending.instance) {
                    ++operation->next_pending_state;
                    (*runNext)();
                    return;
                }

                pending.instance->requestState(StateContextType::Project, false, nullptr,
                                               [operation, complete, runNext, pending](std::vector<uint8_t> state, std::string error, void* callbackContext) mutable {
                                                   (void) callbackContext;
                                                   if (!error.empty()) {
                                                       complete(TimelineFacade::ProjectResult{false, std::format("Failed to retrieve plugin state for instance {}: {}",
                                                                                                                  pending.instance_id, error)});
                                                       return;
                                                   }

                                                   std::string writeError;
                                                   auto relativePath = sequencer_detail::writePluginStateBlob(operation->project_dir,
                                                                                                              operation->plugin_state_dir,
                                                                                                              pending.scope_label,
                                                                                                              pending.plugin_order,
                                                                                                              pending.instance_id,
                                                                                                              state,
                                                                                                              writeError);
                                                   if (!writeError.empty()) {
                                                       complete(TimelineFacade::ProjectResult{false, std::move(writeError)});
                                                       return;
                                                   }

                                                   if (pending.set_state_file)
                                                       pending.set_state_file(relativePath);

                                                   ++operation->next_pending_state;
                                                   (*runNext)();
                                               });
            };
            (*runNext)();
        }

        ClipAddResult addMidiClipToTimelineTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            const std::string& filepath,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool nrpnToParameterMapping,
            bool needsFileSave,
            int32_t requestedClipId = -1) {
            ClipAddResult result;

            // Normalize incoming ticks to the single project-wide PPQ, established by whichever
            // MIDI clip is added first. This keeps every clip's ticks directly comparable, so no
            // clip-to-clip rescaling is needed anywhere else once ticks enter the system.
            uint32_t effectiveResolution = tickResolution == 0 ? 480 : tickResolution;
            if (timeline_.projectTickResolution == 0)
                timeline_.projectTickResolution = effectiveResolution;
            else if (effectiveResolution != timeline_.projectTickResolution)
                MidiClipReader::rescaleTicks(umpTickTimestamps, tempoChanges, timeSignatureChanges,
                    effectiveResolution, timeline_.projectTickResolution);
            tickResolution = timeline_.projectTickResolution;

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<MidiClipSourceNode>(
                sourceNodeId,
                std::move(umpEvents),
                std::move(umpTickTimestamps),
                tickResolution,
                clipTempo,
                static_cast<double>(sampleRate_),
                std::move(tempoChanges),
                std::move(timeSignatureChanges)
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.clipId = requestedClipId;
            clip.referenceId = takePendingClipReferenceId();
            clip.clipType = ClipType::Midi;
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.filepath = filepath;
            clip.needsFileSave = needsFileSave;
            clip.tickResolution = tickResolution;
            clip.clipTempo = clipTempo;
            clip.gain = 1.0;
            clip.muted = false;
            clip.name = clipName.empty() ? "MIDI Clip" : clipName;
            clip.setTimeReference(TimeReference::fromContainerStart({}, position.toSeconds(sampleRate_)), sampleRate_);
            clip.nrpnToParameterMapping = nrpnToParameterMapping;

            int32_t clipId = timelineTrack.addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
                applyAuthoritativeTempoMapToMusicalClips();
                emitClipAdded(timelineTrack, clipId, sourceNodeId);
                emitMasterTrackChanged("master-track-content-changed");
                notifyTimelineChanged();
            } else {
                result.error = "Failed to add MIDI clip to track";
            }
            return result;
        }

        ClipAddResult addAudioClipToTrack(
            TimelineTrack& timelineTrack,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            int32_t requestedClipId = -1) {
            ClipAddResult result;
            if (!reader) {
                result.error = "Invalid audio file reader";
                return result;
            }

            int32_t sourceNodeId = next_source_node_id_++;
            auto sourceNode = std::make_unique<AudioFileSourceNode>(
                sourceNodeId,
                std::move(reader),
                static_cast<double>(sampleRate_),
                audioWarps
            );

            int64_t durationSamples = sourceNode->totalLength();

            ClipData clip;
            clip.clipId = requestedClipId;
            clip.referenceId = takePendingClipReferenceId();
            clip.position = position;
            clip.durationSamples = durationSamples;
            clip.sourceNodeInstanceId = sourceNodeId;
            clip.gain = 1.0;
            clip.muted = false;
            clip.filepath = filepath;
            clip.setTimeReference(TimeReference::fromContainerStart({}, position.toSeconds(sampleRate_)), sampleRate_);
            clip.markers = std::move(markers);
            clip.audioWarps = std::move(audioWarps);

            int32_t clipId = timelineTrack.addClip(clip, std::move(sourceNode));
            if (clipId >= 0) {
                result.success = true;
                result.clipId = clipId;
                result.sourceNodeId = sourceNodeId;
                emitClipAdded(timelineTrack, clipId, sourceNodeId);
                notifyTimelineChanged();
            } else {
                result.error = "Failed to add clip to track";
            }
            return result;
        }

        ClipAddResult addAudioClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::unique_ptr<AudioFileReader> reader,
            const std::string& filepath,
            ProjectMutationOrigin origin) override
        {
            ClipAddResult result;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                result.error = "Invalid track index";
                return result;
            }
            return recordAddedClip(
                trackIndex,
                addAudioClipToTrack(
                    *timeline_tracks_[static_cast<size_t>(trackIndex)],
                    position,
                    std::move(reader),
                    filepath,
                    {},
                    {}),
                origin);
        }

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            const std::string& filepath,
            bool nrpnToParameterMapping,
            ProjectMutationOrigin origin) override
        {
            ClipAddResult result;
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                result.error = "Invalid track index";
                return result;
            }

            auto clipInfo = MidiClipReader::readAnyFormat(filepath);
            if (!clipInfo.success) {
                result.error = clipInfo.error;
                return result;
            }

            auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
            auto& musicalClip = separated.musicalClip;
            auto& track = *timeline_tracks_[static_cast<size_t>(trackIndex)];
            const bool recordsHistory = origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
            const bool ownsCompound = recordsHistory
                && separated.hasMasterTrackClip()
                && !undo_engine_.state().compoundOpen;
            if (ownsCompound) {
                auto opened = undo_engine_.beginCompound("Import MIDI file", origin);
                if (!opened.succeeded()) {
                    result.error = std::move(opened.error);
                    return result;
                }
            }
            result = addMidiClipToTimelineTrack(
                track,
                position,
                filepath,
                std::move(musicalClip.ump_data),
                std::move(musicalClip.ump_tick_timestamps),
                musicalClip.tick_resolution,
                musicalClip.tempo,
                std::move(musicalClip.tempo_changes),
                std::move(musicalClip.time_signature_changes),
                std::filesystem::path(filepath).stem().string(),
                nrpnToParameterMapping,
                separated.hasMasterTrackClip());

            result = recordAddedClip(trackIndex, std::move(result), origin);
            if (!result.success) {
                if (ownsCompound)
                    undo_engine_.cancelCompound();
                return result;
            }

            if (result.success && separated.hasMasterTrackClip()) {
                auto& masterClip = separated.masterTrackClip;
                auto masterResult = addMidiClipToTimelineTrack(
                    *master_timeline_track_,
                    position,
                    "",
                    {},
                    {},
                    masterClip.tick_resolution,
                    masterClip.tempo,
                    std::move(masterClip.tempo_changes),
                    std::move(masterClip.time_signature_changes),
                    std::format("{} Meta", std::filesystem::path(filepath).stem().string()),
                    false,
                    false);
                if (masterResult.success) {
                    if (const auto* regularClip = track.clipManager().getClip(result.clipId)) {
                        if (!master_timeline_track_->clipManager().setClipAnchor(
                            masterResult.clipId,
                            TimeReference::fromContainerStart(regularClip->referenceId, 0.0),
                            sampleRate_)) {
                            removeClipRaw(*master_timeline_track_, masterResult.clipId);
                            masterResult.success = false;
                            masterResult.error = "Could not anchor the imported master MIDI clip";
                        }
                    }
                }
                masterResult = recordAddedClip(
                    kMasterTrackIndex, std::move(masterResult), origin);
                if (!masterResult.success) {
                    if (ownsCompound)
                        undo_engine_.cancelCompound();
                    result.success = false;
                    result.error = masterResult.error.empty()
                        ? "Could not add the imported master MIDI clip"
                        : std::move(masterResult.error);
                    return result;
                }
            }
            if (ownsCompound)
                undo_engine_.endCompound();
            return result;
        }

        ClipAddResult addMidiClipToTrack(
            int32_t trackIndex,
            const TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool nrpnToParameterMapping,
            bool needsFileSave,
            ProjectMutationOrigin origin) override
        {
            if (trackIndex < 0 || trackIndex >= static_cast<int32_t>(timeline_tracks_.size())) {
                ClipAddResult result;
                result.error = "Invalid track index";
                return result;
            }
            return recordAddedClip(
                trackIndex,
                addMidiClipToTimelineTrack(
                    *timeline_tracks_[static_cast<size_t>(trackIndex)],
                    position,
                    "",
                    std::move(umpEvents),
                    std::move(umpTickTimestamps),
                    tickResolution,
                    clipTempo,
                    std::move(tempoChanges),
                    std::move(timeSignatureChanges),
                    clipName,
                    nrpnToParameterMapping,
                    needsFileSave),
                origin);
        }

        ClipAddResult addMasterMidiClip(
            const TimelinePosition& position,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            uint32_t tickResolution,
            double clipTempo,
            std::vector<MidiTempoChange> tempoChanges,
            std::vector<MidiTimeSignatureChange> timeSignatureChanges,
            const std::string& clipName,
            bool needsFileSave,
            const std::string& filepath,
            ProjectMutationOrigin origin) override
        {
            return recordAddedClip(
                kMasterTrackIndex,
                addMidiClipToTimelineTrack(
                    *master_timeline_track_,
                    position,
                    filepath,
                    std::move(umpEvents),
                    std::move(umpTickTimestamps),
                    tickResolution,
                    clipTempo,
                    std::move(tempoChanges),
                    std::move(timeSignatureChanges),
                    clipName,
                    false,
                    needsFileSave),
                origin);
        }

        bool removeClipFromTrack(
            int32_t trackIndex,
            int32_t clipId,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote)
                return removeClipRaw(*targetTrack, clipId);

            // Capture before beginning any document transaction: ARA archives
            // may not be created while the document is being edited.
            auto fragment = captureClipFragment(trackIndex, clipId);
            if (!fragment)
                return false;
            return performCapturedClipRemoval(
                targetTrack->referenceId(), std::move(*fragment), origin);
        }

        bool clearClipsFromTrack(
            int32_t trackIndex,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            const auto clips = targetTrack->clipManager().getAllClips();
            if (clips.empty())
                return false;

            if (origin != ProjectMutationOrigin::User && origin != ProjectMutationOrigin::Remote) {
                ProjectDocumentTransaction transaction(project_document_events_);
                bool removedAny = false;
                for (const auto& clip : clips)
                    removedAny |= removeClipRaw(*targetTrack, clip.clipId);
                return removedAny;
            }

            std::vector<ProjectClipFragment> fragments;
            fragments.reserve(clips.size());
            for (const auto& clip : clips) {
                auto fragment = captureClipFragment(trackIndex, clip.clipId);
                if (!fragment)
                    return false;
                fragments.push_back(std::move(*fragment));
            }

            const bool ownsCompound = !undo_engine_.state().compoundOpen;
            if (ownsCompound) {
                auto opened = undo_engine_.beginCompound("Clear clips", origin);
                if (!opened.succeeded())
                    return false;
            }
            // Removed one at a time rather than by clearing the clip manager,
            // so that every clip produces its own removal event. Clearing
            // directly leaves observers holding clips that no longer exist.
            ProjectDocumentTransaction transaction(project_document_events_);
            for (auto& fragment : fragments) {
                if (performCapturedClipRemoval(
                        targetTrack->referenceId(), std::move(fragment), origin))
                    continue;
                if (ownsCompound)
                    undo_engine_.cancelCompound();
                return false;
            }
            if (ownsCompound)
                undo_engine_.endCompound();
            return true;
        }

        bool setTrackGain(
            int32_t trackIndex,
            double gain,
            ProjectMutationOrigin origin) override {
            return setUndoableTrackProperty(
                trackIndex, gain, origin,
                "Change track gain", "track-gain-changed",
                [](const SequencerTrack& track) { return track.trackGain(); },
                [](SequencerTrack& track, double value) {
                    return track.trackGain(value);
                });
        }

        bool setTrackMuted(
            int32_t trackIndex,
            bool muted,
            ProjectMutationOrigin origin) override {
            return setUndoableTrackProperty(
                trackIndex, muted, origin,
                muted ? "Mute track" : "Unmute track", "track-mute-changed",
                [](const SequencerTrack& track) { return track.muted(); },
                [](SequencerTrack& track, bool value) {
                    track.muted(value);
                    return true;
                });
        }

        bool setTrackSolo(
            int32_t trackIndex,
            bool solo,
            ProjectMutationOrigin origin) override {
            return setUndoableTrackProperty(
                trackIndex, solo, origin,
                solo ? "Solo track" : "Unsolo track", "track-solo-changed",
                [](const SequencerTrack& track) { return track.solo(); },
                [](SequencerTrack& track, bool value) {
                    track.solo(value);
                    return true;
                });
        }

        bool setTrackBypassed(
            int32_t trackIndex,
            bool bypassed,
            ProjectMutationOrigin origin) override {
            return setUndoableTrackProperty(
                trackIndex, bypassed, origin,
                bypassed ? "Bypass track" : "Enable track processing",
                "track-bypass-changed",
                [](SequencerTrack& track) { return track.bypassed(); },
                [](SequencerTrack& track, bool value) {
                    track.bypassed(value);
                    return true;
                });
        }

        bool setTrackFreezePolicyEnabled(
            int32_t trackIndex,
            bool enabled,
            ProjectMutationOrigin origin) override {
            auto* timelineTrack = resolveTrack(trackIndex);
            if (!timelineTrack || trackIndex == kMasterTrackIndex)
                return false;
            auto& manager = engine_.frozenTrackManager();
            const bool before = manager.freezePolicyForTrack(trackIndex)
                == FrozenTrackManager::FreezePolicy::On;
            if (before == enabled)
                return true;

            auto trackReferenceId = timelineTrack->referenceId();
            auto apply = [this](
                             std::string_view persistentTrackId,
                             const bool& value) {
                const auto currentIndex =
                    trackIndexForPersistentId(persistentTrackId);
                if (currentIndex < 0)
                    return false;
                if (!engine_.frozenTrackManager().setFreezePolicyForTrack(
                        currentIndex,
                        value
                            ? FrozenTrackManager::FreezePolicy::On
                            : FrozenTrackManager::FreezePolicy::Off))
                    return false;
                emitTrackChanged(
                    persistentTrackId,
                    "track-freeze-policy-changed");
                return true;
            };
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return apply(trackReferenceId, enabled);

            auto operation = std::make_shared<TrackPropertyUndoOperation<bool>>(
                enabled ? "Freeze track" : "Unfreeze track",
                "track-freeze-policy-changed",
                std::move(trackReferenceId),
                before,
                enabled,
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        bool setLatencyCompensationSettings(
            const LatencyCompensationProjectSettings& settings,
            ProjectMutationOrigin origin) override {
            auto* manager = engine_.latencyCompensationManager();
            if (!manager)
                return false;
            auto before = manager->projectSettings();
            if (latencyCompensationSettingsEqual(before, settings))
                return true;

            auto apply = [this](
                             const LatencyCompensationProjectSettings& value,
                             std::string& error) {
                auto* currentManager = engine_.latencyCompensationManager();
                if (!currentManager
                    || !currentManager->applyProjectSettings(value, error))
                    return false;
                notifyTimelineChanged();
                return true;
            };
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote) {
                std::string error;
                return apply(settings, error);
            }

            auto operation = std::make_shared<LatencySettingsUndoOperation>(
                std::move(before),
                settings,
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        bool applyDeviceInputState(
            std::string_view trackReferenceId,
            int32_t sourceNodeId,
            const DeviceInputUndoOperation::Channels& channels) {
            auto* targetTrack = resolveTrackByReferenceId(trackReferenceId);
            if (!targetTrack)
                return false;
            auto source = targetTrack->getSourceNode(sourceNodeId);
            auto deviceInput =
                std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
            if (!channels) {
                if (!deviceInput || !targetTrack->removeSource(sourceNodeId))
                    return false;
            } else if (deviceInput)
                deviceInput->setInputChannels(*channels);
            else {
                if (source)
                    return false;
                const auto channelCount = static_cast<uint32_t>(channels->size());
                auto newSource = std::make_unique<DeviceInputSourceNode>(
                    sourceNodeId,
                    channelCount,
                    *channels);
                if (!targetTrack->addDeviceInputSource(std::move(newSource)))
                    return false;
            }
            emitTrackChanged(trackReferenceId, "track-device-input-changed");
            notifyTimelineChanged();
            return true;
        }

        bool performDeviceInputMutation(
            int32_t trackIndex,
            int32_t sourceNodeId,
            DeviceInputUndoOperation::Channels before,
            DeviceInputUndoOperation::Channels after,
            ProjectMutationOrigin origin,
            std::string description) {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            auto trackReferenceId = targetTrack->referenceId();
            auto apply = [this](
                             std::string_view persistentTrackId,
                             int32_t persistentSourceNodeId,
                             const DeviceInputUndoOperation::Channels& value) {
                return applyDeviceInputState(
                    persistentTrackId,
                    persistentSourceNodeId,
                    value);
            };
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return apply(trackReferenceId, sourceNodeId, after);

            auto operation = std::make_shared<DeviceInputUndoOperation>(
                std::move(description),
                std::move(trackReferenceId),
                sourceNodeId,
                std::move(before),
                std::move(after),
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        bool addDeviceInputToTrack(
            int32_t trackIndex,
            int32_t sourceNodeId,
            const std::vector<uint32_t>& channelIndices,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack || targetTrack->getSourceNode(sourceNodeId))
                return false;
            auto normalizedChannels = channelIndices;
            if (normalizedChannels.empty())
                normalizedChannels = {0, 1};
            return performDeviceInputMutation(
                trackIndex,
                sourceNodeId,
                std::nullopt,
                std::move(normalizedChannels),
                origin,
                "Add device input");
        }

        bool setDeviceInputChannels(
            int32_t trackIndex,
            int32_t sourceNodeId,
            const std::vector<uint32_t>& channelIndices,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            auto source = targetTrack
                ? targetTrack->getSourceNode(sourceNodeId)
                : nullptr;
            auto deviceInput =
                std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
            if (!deviceInput)
                return false;
            return performDeviceInputMutation(
                trackIndex,
                sourceNodeId,
                deviceInput->getInputChannels(),
                channelIndices,
                origin,
                "Change device input routing");
        }

        bool removeDeviceInputFromTrack(
            int32_t trackIndex,
            int32_t sourceNodeId,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            auto source = targetTrack
                ? targetTrack->getSourceNode(sourceNodeId)
                : nullptr;
            auto deviceInput =
                std::dynamic_pointer_cast<DeviceInputSourceNode>(source);
            if (!deviceInput)
                return false;
            return performDeviceInputMutation(
                trackIndex,
                sourceNodeId,
                deviceInput->getInputChannels(),
                std::nullopt,
                origin,
                "Remove device input");
        }

        struct PluginTarget {
            std::string trackReferenceId;
            std::string nodeId;
        };

        std::optional<PluginTarget> pluginTargetForInstance(
            int32_t instanceId) {
            const auto trackIndex = engine_.findTrackIndexForInstance(instanceId);
            auto* timelineTrack = resolveTrack(trackIndex);
            auto* sequencerTrack = resolveSequencerTrack(trackIndex);
            auto* node = sequencerTrack
                ? sequencerTrack->graph().getPluginNode(instanceId)
                : nullptr;
            if (!timelineTrack || !node || node->nodeId().empty())
                return std::nullopt;
            return PluginTarget{
                .trackReferenceId = timelineTrack->referenceId(),
                .nodeId = node->nodeId()
            };
        }

        int32_t resolvePluginInstanceId(
            std::string_view trackReferenceId,
            std::string_view nodeId) {
            auto* track = resolveSequencerTrackByReferenceId(trackReferenceId);
            if (!track)
                return -1;
            for (const auto instanceId : track->orderedInstanceIds()) {
                auto node = track->graph().getPluginNode(instanceId);
                if (node && node->nodeId() == nodeId)
                    return instanceId;
            }
            return -1;
        }

        bool applyPluginParameterValue(
            std::string_view persistentTrackId,
            std::string_view persistentNodeId,
            int32_t parameterIndex,
            double value) {
            const auto currentInstanceId = resolvePluginInstanceId(
                persistentTrackId,
                persistentNodeId);
            auto* currentInstance = engine_.getPluginInstance(currentInstanceId);
            if (!currentInstance
                || engine_.frozenTrackManager().isInstanceBusy(currentInstanceId))
                return false;
            plugin_parameter_mutation_depth_.fetch_add(1, std::memory_order_acq_rel);
            engine_.setParameterValue(currentInstanceId, parameterIndex, value);
            plugin_parameter_mutation_depth_.fetch_sub(1, std::memory_order_acq_rel);
            plugin_parameter_values_[currentInstanceId][parameterIndex] = value;
            emitTrackChanged(persistentTrackId, std::format(
                "plugin-parameter-{}-changed", parameterIndex));
            notifyTimelineChanged();
            return true;
        }

        void recordExternalPluginParameterChange(
            int32_t instanceId,
            int32_t parameterIndex,
            double before,
            double after) {
            auto target = pluginTargetForInstance(instanceId);
            if (!target || before == after)
                return;
            auto operation = std::make_shared<PluginPropertyUndoOperation<double>>(
                "Change plug-in parameter",
                std::format("plugin-parameter-{}-changed", parameterIndex),
                target->trackReferenceId,
                target->nodeId,
                before,
                after,
                [this, parameterIndex](
                    std::string_view persistentTrackId,
                    std::string_view persistentNodeId,
                    const double& value) {
                    return applyPluginParameterValue(
                        persistentTrackId,
                        persistentNodeId,
                        parameterIndex,
                        value);
                });
            undo_engine_.recordPerformed(
                std::move(operation),
                ProjectMutationOrigin::User);
            emitTrackChanged(
                target->trackReferenceId,
                std::format("plugin-parameter-{}-changed", parameterIndex));
            notifyTimelineChanged();
        }

        void onPluginParameterChanged(
            int32_t instanceId,
            uint32_t parameterIndex,
            double value) {
            if (plugin_parameter_mutation_depth_.load(std::memory_order_acquire) != 0)
                return;
            if (!remidy::EventLoop::runningOnMainThread()) {
                dispatchToModelThread([this, instanceId, parameterIndex, value] {
                    onPluginParameterChanged(instanceId, parameterIndex, value);
                });
                return;
            }
            const auto index = static_cast<int32_t>(parameterIndex);
            auto& values = plugin_parameter_values_[instanceId];
            const auto previous = values.find(index);
            const auto before = previous == values.end() ? value : previous->second;
            values[index] = value;
            if (previous == values.end())
                return;
            recordExternalPluginParameterChange(instanceId, index, before, value);
        }

        void pluginInstanceAdded(
            int32_t instanceId,
            AudioPluginInstanceAPI& instance) override {
            auto& values = plugin_parameter_values_[instanceId];
            for (const auto& parameter : instance.parameterMetadataList())
                values[static_cast<int32_t>(parameter.index)] =
                    instance.getParameterValue(static_cast<int32_t>(parameter.index));
            auto* support = instance.parameterSupport();
            if (!support)
                return;
            plugin_parameter_listener_ids_[instanceId] =
                support->parameterChangeEvent().addListener(
                [this, instanceId](uint32_t parameterIndex, double value) {
                    onPluginParameterChanged(instanceId, parameterIndex, value);
                });
        }

        void pluginInstanceWillBeDestroyed(int32_t instanceId) override {
            const auto listener = plugin_parameter_listener_ids_.find(instanceId);
            if (listener != plugin_parameter_listener_ids_.end()) {
                if (auto* instance = engine_.getPluginInstance(instanceId)) {
                    if (auto* support = instance->parameterSupport())
                        support->parameterChangeEvent().removeListener(listener->second);
                }
                plugin_parameter_listener_ids_.erase(listener);
            }
            plugin_parameter_values_.erase(instanceId);
        }

        template<typename Value, typename Read, typename Mutation>
        bool setUndoablePluginProperty(
            int32_t instanceId,
            std::string propertyKey,
            Value after,
            ProjectMutationOrigin origin,
            std::string description,
            Read&& read,
            Mutation&& mutate) {
            auto target = pluginTargetForInstance(instanceId);
            auto* instance = engine_.getPluginInstance(instanceId);
            if (!target || !instance)
                return false;
            Value before = read(*instance);
            if (before == after)
                return true;

            auto changeType = propertyKey;
            auto apply = [this,
                          changeType = std::move(changeType),
                          mutate = std::forward<Mutation>(mutate)](
                             std::string_view persistentTrackId,
                             std::string_view persistentNodeId,
                             const Value& value) mutable {
                const auto currentInstanceId = resolvePluginInstanceId(
                    persistentTrackId,
                    persistentNodeId);
                auto* currentInstance =
                    engine_.getPluginInstance(currentInstanceId);
                if (!currentInstance
                    || !mutate(currentInstanceId, *currentInstance, value))
                    return false;
                emitTrackChanged(persistentTrackId, changeType);
                notifyTimelineChanged();
                return true;
            };
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return apply(target->trackReferenceId, target->nodeId, after);

            auto operation = std::make_shared<PluginPropertyUndoOperation<Value>>(
                std::move(description),
                std::move(propertyKey),
                std::move(target->trackReferenceId),
                std::move(target->nodeId),
                std::move(before),
                std::move(after),
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        bool setPluginBypassed(
            int32_t instanceId,
            bool bypassed,
            ProjectMutationOrigin origin) override {
            return setUndoablePluginProperty(
                instanceId,
                "plugin-bypass-changed",
                bypassed,
                origin,
                bypassed ? "Bypass plug-in" : "Enable plug-in",
                [](const AudioPluginInstanceAPI& instance) {
                    return instance.bypassed();
                },
                [](int32_t, AudioPluginInstanceAPI& instance, bool value) {
                    instance.bypassed(value);
                    return true;
                });
        }

        bool setPluginParameterValue(
            int32_t instanceId,
            int32_t parameterIndex,
            double value,
            ProjectMutationOrigin origin) override {
            return setUndoablePluginProperty(
                instanceId,
                std::format("plugin-parameter-{}-changed", parameterIndex),
                value,
                origin,
                "Change plug-in parameter",
                [parameterIndex](AudioPluginInstanceAPI& instance) {
                    return instance.getParameterValue(parameterIndex);
                },
                [this, parameterIndex](
                    int32_t currentInstanceId,
                    AudioPluginInstanceAPI&,
                    double currentValue) {
                    const auto target = pluginTargetForInstance(currentInstanceId);
                    return target
                        && applyPluginParameterValue(
                            target->trackReferenceId,
                            target->nodeId,
                            parameterIndex,
                            currentValue);
                });
        }

        bool setPluginPerNoteControllerValue(
            int32_t instanceId,
            remidy::PerNoteControllerContextTypes contextType,
            remidy::PerNoteControllerContext context,
            int32_t parameterIndex,
            double value,
            ProjectMutationOrigin origin) override {
            auto* instance = engine_.getPluginInstance(instanceId);
            if (!instance || parameterIndex < 0)
                return false;

            auto* parameterSupport = instance->parameterSupport();
            double before = std::numeric_limits<double>::quiet_NaN();
            if (parameterSupport
                && parameterSupport->getPerNoteController(
                       context,
                       static_cast<uint32_t>(parameterIndex),
                       &before)
                    == remidy::StatusCode::OK) {
                // The plug-in supplied a readable value for the requested
                // context; keep using the support API for both directions.
            } else if (contextType
                       == remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE
                       && context.note <= UINT8_MAX
                       && parameterIndex <= UINT8_MAX
                       && instance->getPerNoteControllerValue(
                           static_cast<uint8_t>(context.note),
                           static_cast<uint8_t>(parameterIndex),
                           &before)) {
                // Older adapters expose note-scoped values only through the
                // AudioPluginInstanceAPI convenience method.
            } else {
                return false;
            }
            if (!std::isfinite(before) || before == value)
                return true;

            const auto target = pluginTargetForInstance(instanceId);
            if (!target)
                return false;
            const auto propertyKey = std::format(
                "plugin-per-note-{}-{}-{}-{}-{}-changed",
                static_cast<int>(contextType),
                context.note,
                context.channel,
                context.group,
                parameterIndex);
            const auto description = "Change per-note plug-in parameter";
            auto apply = [this,
                          contextType,
                          context,
                          parameterIndex](
                             std::string_view persistentTrackId,
                             std::string_view persistentNodeId,
                             const double& currentValue) {
                const auto currentInstanceId = resolvePluginInstanceId(
                    persistentTrackId,
                    persistentNodeId);
                auto* currentInstance = engine_.getPluginInstance(currentInstanceId);
                if (!currentInstance
                    || engine_.frozenTrackManager().isInstanceBusy(currentInstanceId))
                    return false;
                auto* support = currentInstance->parameterSupport();
                if (support
                    && support->setPerNoteController(
                           context,
                           static_cast<uint32_t>(parameterIndex),
                           currentValue)
                        == remidy::StatusCode::OK) {
                    emitTrackChanged(persistentTrackId, "plugin-per-note-parameter-changed");
                    notifyTimelineChanged();
                    return true;
                }
                if (contextType
                        == remidy::PerNoteControllerContextTypes::PER_NOTE_CONTROLLER_PER_NOTE) {
                    if (context.note > UINT8_MAX || parameterIndex > UINT8_MAX)
                        return false;
                    currentInstance->setPerNoteControllerValue(
                        static_cast<uint8_t>(context.note),
                        static_cast<uint8_t>(parameterIndex),
                        currentValue);
                    emitTrackChanged(persistentTrackId, "plugin-per-note-parameter-changed");
                    notifyTimelineChanged();
                    return true;
                }
                return false;
            };
            auto operation = std::make_shared<PluginPropertyUndoOperation<double>>(
                description,
                propertyKey,
                target->trackReferenceId,
                target->nodeId,
                before,
                value,
                std::move(apply));
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote) {
                std::optional<ProjectUndoResult> result;
                operation->perform(
                    ProjectUndoExecutionContext{.origin = origin},
                    [&result](ProjectUndoResult completed) {
                        result = std::move(completed);
                    });
                return result.has_value() && result->succeeded();
            }
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            return result->has_value() && result->value().succeeded();
        }

        bool setPluginGroup(
            int32_t instanceId,
            uint8_t group,
            ProjectMutationOrigin origin) override {
            const auto currentGroup = engine_.getInstanceGroup(instanceId);
            if (currentGroup == 0xFF)
                return false;
            return setUndoablePluginProperty(
                instanceId,
                "plugin-group-changed",
                group,
                origin,
                "Change plug-in UMP group",
                [currentGroup](const AudioPluginInstanceAPI&) {
                    return currentGroup;
                },
                [this](int32_t currentInstanceId, AudioPluginInstanceAPI&, uint8_t value) {
                    return engine_.setInstanceGroup(currentInstanceId, value);
                });
        }

        void applyPluginState(
            std::string trackReferenceId,
            std::string nodeId,
            std::vector<uint8_t> state,
            ProjectUndoCompletion completion) {
            const auto instanceId = resolvePluginInstanceId(
                trackReferenceId,
                nodeId);
            auto* instance = engine_.getPluginInstance(instanceId);
            if (!instance) {
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "The plug-in instance no longer exists"));
                return;
            }
            instance->loadState(
                std::move(state),
                StateContextType::Project,
                false,
                nullptr,
                [this,
                 trackReferenceId = std::move(trackReferenceId),
                 completion = std::move(completion)](
                    std::string error, void*) mutable {
                    dispatchToModelThread(
                        [this,
                         trackReferenceId = std::move(trackReferenceId),
                         error = std::move(error),
                         completion = std::move(completion)]() mutable {
                            if (!error.empty()) {
                                if (completion)
                                    completion(ProjectUndoResult::failure(
                                        std::move(error)));
                                return;
                            }
                            emitTrackChanged(
                                trackReferenceId,
                                "plugin-state-changed");
                            notifyTimelineChanged();
                            if (completion)
                                completion(ProjectUndoResult::success());
                        });
                });
        }

        void setPluginState(
            int32_t instanceId,
            std::vector<uint8_t> state,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) override {
            auto target = pluginTargetForInstance(instanceId);
            auto* instance = engine_.getPluginInstance(instanceId);
            if (!target || !instance) {
                if (completion)
                    completion(ProjectUndoResult::failure(
                        "The plug-in instance does not exist"));
                return;
            }
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote) {
                applyPluginState(
                    std::move(target->trackReferenceId),
                    std::move(target->nodeId),
                    std::move(state),
                    std::move(completion));
                return;
            }

            auto after = std::make_shared<std::vector<uint8_t>>(
                std::move(state));
            auto persistentTarget =
                std::make_shared<PluginTarget>(std::move(*target));
            instance->requestState(
                StateContextType::Project,
                false,
                nullptr,
                [this,
                 origin,
                 after,
                 persistentTarget,
                 completion = std::move(completion)](
                    std::vector<uint8_t> before,
                    std::string error,
                    void*) mutable {
                    dispatchToModelThread(
                        [this,
                         origin,
                         before = std::move(before),
                         error = std::move(error),
                         after,
                         persistentTarget,
                         completion = std::move(completion)]() mutable {
                            if (!error.empty()) {
                                if (completion)
                                    completion(ProjectUndoResult::failure(
                                        std::move(error)));
                                return;
                            }
                            auto apply = [this](
                                             std::string_view trackReferenceId,
                                             std::string_view nodeId,
                                             const std::vector<uint8_t>& value,
                                             ProjectUndoCompletion applied) {
                                applyPluginState(
                                    std::string(trackReferenceId),
                                    std::string(nodeId),
                                    value,
                                    std::move(applied));
                            };
                            auto operation =
                                std::make_shared<PluginStateUndoOperation>(
                                    persistentTarget->trackReferenceId,
                                    persistentTarget->nodeId,
                                    std::move(before),
                                    std::move(*after),
                                    std::move(apply));
                            undo_engine_.perform(
                                std::move(operation),
                                origin,
                                std::move(completion));
                        });
                });
        }

        static bool graphEndpointEquivalent(
            const uapmd_graph::AudioPluginGraphEndpoint& lhs,
            const uapmd_graph::AudioPluginGraphEndpoint& rhs) {
            return lhs.type == rhs.type
                && lhs.node_id == rhs.node_id
                && lhs.bus_index == rhs.bus_index;
        }

        static bool graphConnectionEquivalent(
            const uapmd_graph::AudioPluginGraphConnection& lhs,
            const uapmd_graph::AudioPluginGraphConnection& rhs) {
            return lhs.bus_type == rhs.bus_type
                && graphEndpointEquivalent(lhs.source, rhs.source)
                && graphEndpointEquivalent(lhs.target, rhs.target);
        }

        bool applyGraphConnectionState(
            std::string_view trackReferenceId,
            const uapmd_graph::AudioPluginGraphConnection& desired,
            bool present,
            std::string& error) {
            const auto trackIndex = trackIndexForPersistentId(trackReferenceId);
            auto* track = resolveSequencerTrack(trackIndex);
            auto* graph = track
                ? dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(&track->graph())
                : nullptr;
            if (!graph) {
                error = "Track graph is not a full DAG graph";
                return false;
            }

            const auto connections = graph->connections();
            auto existing = std::find_if(
                connections.begin(),
                connections.end(),
                [&desired](const auto& connection) {
                    return graphConnectionEquivalent(connection, desired);
                });
            if (!present) {
                if (existing == connections.end()) {
                    error = "Graph connection not found";
                    return false;
                }
                if (!graph->disconnect(existing->id)) {
                    error = "Failed to disconnect graph endpoints";
                    return false;
                }
            } else {
                if (existing != connections.end())
                    return true;
                auto connection = desired;
                connection.id = 0;
                auto resolveEndpoint = [this, trackReferenceId](
                                           auto& endpoint) {
                    if (endpoint.type
                        != uapmd_graph::AudioPluginGraphEndpointType::Plugin)
                        return true;
                    endpoint.instance_id = resolvePluginInstanceId(
                        trackReferenceId,
                        endpoint.node_id);
                    return endpoint.instance_id >= 0;
                };
                if (!resolveEndpoint(connection.source)
                    || !resolveEndpoint(connection.target)) {
                    error = "A plug-in graph endpoint no longer exists";
                    return false;
                }
                const auto result = graph->connect(connection);
                if (result != 0) {
                    if (result == -1)
                        error = "Invalid graph endpoint direction";
                    else if (result == -2)
                        error = "Graph endpoint does not exist";
                    else if (result == -3)
                        error = "Graph connection would create a cycle";
                    else
                        error = "Failed to connect graph endpoints";
                    return false;
                }
            }
            onTrackGraphChanged(trackIndex);
            notifyTimelineChanged();
            return true;
        }

        bool performGraphConnectionMutation(
            int32_t trackIndex,
            uapmd_graph::AudioPluginGraphConnection connection,
            bool present,
            std::string& error,
            ProjectMutationOrigin origin) {
            auto* timelineTrack = resolveTrack(trackIndex);
            if (!timelineTrack) {
                error = "Track not found";
                return false;
            }
            auto trackReferenceId = timelineTrack->referenceId();
            auto apply = [this](
                             std::string_view persistentTrackId,
                             const uapmd_graph::AudioPluginGraphConnection& value,
                             bool desiredPresence,
                             std::string& applyError) {
                return applyGraphConnectionState(
                    persistentTrackId,
                    value,
                    desiredPresence,
                    applyError);
            };
            if (origin != ProjectMutationOrigin::User
                && origin != ProjectMutationOrigin::Remote)
                return apply(trackReferenceId, connection, present, error);

            auto operation = std::make_shared<GraphConnectionUndoOperation>(
                present,
                std::move(trackReferenceId),
                std::move(connection),
                std::move(apply));
            auto result = std::make_shared<std::optional<ProjectUndoResult>>();
            undo_engine_.perform(
                std::move(operation),
                origin,
                [result](ProjectUndoResult completed) {
                    *result = std::move(completed);
                });
            if (!result->has_value()) {
                error = "The undo engine did not complete the graph mutation inline";
                return false;
            }
            if (!result->value().succeeded()) {
                error = result->value().error;
                return false;
            }
            return true;
        }

        bool connectTrackGraph(
            int32_t trackIndex,
            const uapmd_graph::AudioPluginGraphConnection& connection,
            std::string& error,
            ProjectMutationOrigin origin) override {
            return performGraphConnectionMutation(
                trackIndex,
                connection,
                true,
                error,
                origin);
        }

        bool disconnectTrackGraphConnection(
            int32_t trackIndex,
            int64_t connectionId,
            std::string& error,
            ProjectMutationOrigin origin) override {
            auto* track = resolveSequencerTrack(trackIndex);
            auto* graph = track
                ? dynamic_cast<uapmd_graph::AudioPluginFullDAGraph*>(&track->graph())
                : nullptr;
            if (!graph) {
                error = "Track graph is not a full DAG graph";
                return false;
            }
            const auto connections = graph->connections();
            auto connection = std::find_if(
                connections.begin(),
                connections.end(),
                [connectionId](const auto& candidate) {
                    return candidate.id == connectionId;
                });
            if (connection == connections.end()) {
                error = "Connection not found";
                return false;
            }
            return performGraphConnectionMutation(
                trackIndex,
                *connection,
                false,
                error,
                origin);
        }

        bool notifyClipChanged(int32_t trackIndex, int32_t clipId, std::string type = "clip-changed") override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            auto* clip = targetTrack->clipManager().getClip(clipId);
            if (!clip)
                return false;
            emitClipChanged(*targetTrack, *clip, std::move(type));
            if (clip->clipType == ClipType::Midi)
                emitMasterTrackChanged("master-track-content-changed");
            return true;
        }

        // Applies `mutate` to the addressed track's clip manager and, when it
        // reports a change, emits `changeType` and refreshes the timeline.
        // Every clip mutator goes through here so that no path can change a
        // clip without the matching document event being emitted.
        template<typename Mutation>
        bool mutateClip(int32_t trackIndex, int32_t clipId, std::string changeType, Mutation&& mutate) {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack || !mutate(targetTrack->clipManager()))
                return false;
            notifyClipChanged(trackIndex, clipId, std::move(changeType));
            notifyTimelineChanged();
            return true;
        }

        bool setClipEnabled(
            int32_t trackIndex,
            int32_t clipId,
            bool enabled,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, enabled, origin,
                enabled ? "Enable clip" : "Disable clip",
                "clip-enablement-changed",
                [](const ClipData& clip) { return clip.enabled; },
                [](ClipManager& clips, int32_t currentClipId, bool value) {
                    return clips.setClipEnabled(currentClipId, value);
                });
        }

        bool setClipAnchor(
            int32_t trackIndex,
            int32_t clipId,
            const TimeReference& anchor,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, anchor, origin,
                "Move clip", "clip-position-changed",
                [this](const ClipData& clip) { return clip.timeReference(sampleRate_); },
                [this](ClipManager& clips, int32_t currentClipId, const TimeReference& value) {
                    if (!clips.setClipAnchor(currentClipId, value, sampleRate_))
                        return false;
                    resolveAllClipAnchors();
                    return true;
                });
        }

        bool setClipGain(
            int32_t trackIndex,
            int32_t clipId,
            double gain,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, gain, origin,
                "Change clip gain", "clip-gain-changed",
                [](const ClipData& clip) { return clip.gain; },
                [](ClipManager& clips, int32_t currentClipId, double value) {
                    return clips.setClipGain(currentClipId, value);
                });
        }

        bool setClipMuted(
            int32_t trackIndex,
            int32_t clipId,
            bool muted,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, muted, origin,
                muted ? "Mute clip" : "Unmute clip", "clip-mute-changed",
                [](const ClipData& clip) { return clip.muted; },
                [](ClipManager& clips, int32_t currentClipId, bool value) {
                    return clips.setClipMuted(currentClipId, value);
                });
        }

        bool resizeClip(
            int32_t trackIndex,
            int32_t clipId,
            int64_t newDurationSamples,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, newDurationSamples, origin,
                "Resize clip", "clip-duration-changed",
                [](const ClipData& clip) { return clip.durationSamples; },
                [](ClipManager& clips, int32_t currentClipId, int64_t value) {
                    return clips.resizeClip(currentClipId, value);
                });
        }

        bool setClipName(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& name,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, name, origin,
                "Rename clip", "clip-name-changed",
                [](const ClipData& clip) { return clip.name; },
                [](ClipManager& clips, int32_t currentClipId, const std::string& value) {
                    return clips.setClipName(currentClipId, value);
                });
        }

        bool setClipFilepath(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& filepath,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, filepath, origin,
                "Change clip file", "clip-content-changed",
                [](const ClipData& clip) { return clip.filepath; },
                [](ClipManager& clips, int32_t currentClipId, const std::string& value) {
                    return clips.setClipFilepath(currentClipId, value);
                });
        }

        bool setClipNeedsFileSave(
            int32_t trackIndex,
            int32_t clipId,
            bool needsSave,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, needsSave, origin,
                "Change clip save state", "clip-content-changed",
                [](const ClipData& clip) { return clip.needsFileSave; },
                [](ClipManager& clips, int32_t currentClipId, bool value) {
                    return clips.setClipNeedsFileSave(currentClipId, value);
                });
        }

        bool setClipMarkers(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<ClipMarker> markers,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, std::move(markers), origin,
                "Edit clip markers", "clip-content-changed",
                [](const ClipData& clip) { return clip.markers; },
                [](ClipManager& clips, int32_t currentClipId, const std::vector<ClipMarker>& value) {
                    return clips.setClipMarkers(currentClipId, value);
                },
                clipMarkersEqual);
        }

        bool setClipAudioWarps(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<AudioWarpPoint> audioWarps,
            ProjectMutationOrigin origin) override {
            return setUndoableClipProperty(
                trackIndex, clipId, std::move(audioWarps), origin,
                "Edit clip warps", "clip-content-changed",
                [](const ClipData& clip) { return clip.audioWarps; },
                [](ClipManager& clips, int32_t currentClipId, const std::vector<AudioWarpPoint>& value) {
                    return clips.setAudioWarps(currentClipId, value);
                },
                audioWarpPointsEqual);
        }

        bool clipEnabled(int32_t trackIndex, int32_t clipId) const override {
            const auto* targetTrack = resolveTrack(trackIndex);
            const auto* clip = targetTrack ? targetTrack->clipManager().getClip(clipId) : nullptr;
            return clip ? clip->enabled : false;
        }

        // Every clip in the project, addressable by reference identifier. Warp
        // and marker references may point at any clip, so resolution needs the
        // whole set.
        ClipReferenceMap buildClipReferenceMap() const {
            ClipReferenceMap lookup;
            auto collect = [&lookup](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return;
                for (auto& clip : track->clipManager().getAllClips())
                    lookup[clip.referenceId] = std::move(clip);
            };
            collect(master_timeline_track_);
            for (const auto& track : timeline_tracks_)
                collect(track);
            return lookup;
        }

        bool replaceAudioClipContent(
            int32_t trackIndex,
            int32_t clipId,
            const std::string& filepath,
            std::vector<ClipMarker> markers,
            std::vector<AudioWarpPoint> audioWarps,
            const std::vector<ClipMarker>& masterTrackMarkers,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            auto* clip = targetTrack->clipManager().getClip(clipId);
            if (!clip || clip->clipType != ClipType::Audio)
                return false;

            std::optional<ProjectClipFragment> before;
            const bool recordsHistory = origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
            if (recordsHistory) {
                before = captureClipFragment(trackIndex, clipId);
                if (!before)
                    return false;
            }

            const auto sourcePath = filepath.empty() ? clip->filepath : filepath;
            auto reader = createAudioFileReaderFromPath(sourcePath);
            if (!reader)
                return false;

            // Resolved against the clip as it will be, not as it is: a warp may
            // reference a marker that this same call is adding.
            auto lookup = buildClipReferenceMap();
            auto target = *clip;
            target.markers = markers;
            lookup[target.referenceId] = target;
            auto resolvedWarps = resolveAudioWarpPoints(
                target, audioWarps, lookup, masterTrackMarkers, static_cast<double>(sampleRate_));

            auto replacement = std::make_unique<AudioFileSourceNode>(
                clip->sourceNodeInstanceId,
                std::move(reader),
                static_cast<double>(sampleRate_),
                std::move(resolvedWarps));
            const int64_t sourceDuration = replacement->totalLength();

            {
                ProjectDocumentTransaction transaction(project_document_events_);
                if (!targetTrack->replaceClipSourceNode(clipId, std::move(replacement)))
                    return false;
                auto& clips = targetTrack->clipManager();
                clips.setClipMarkers(clipId, std::move(markers));
                clips.setAudioWarps(clipId, std::move(audioWarps));
                if (!filepath.empty()) {
                    clips.setClipFilepath(clipId, filepath);
                    clips.resizeClip(clipId, sourceDuration);
                }
                notifyClipChanged(trackIndex, clipId, "clip-content-changed");
                notifyTimelineChanged();
            }
            return !recordsHistory || recordReplacedClip(
                trackIndex, std::move(*before), origin, "Edit audio clip content");
        }

        bool replaceMidiClipContent(
            int32_t trackIndex,
            int32_t clipId,
            std::vector<uapmd_ump_t> umpEvents,
            std::vector<uint64_t> umpTickTimestamps,
            ProjectMutationOrigin origin) override {
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return false;
            auto* clip = targetTrack->clipManager().getClip(clipId);
            if (!clip || clip->clipType != ClipType::Midi)
                return false;
            auto existing = std::dynamic_pointer_cast<MidiClipSourceNode>(
                targetTrack->getSourceNode(clip->sourceNodeInstanceId));
            if (!existing)
                return false;

            std::optional<ProjectClipFragment> before;
            const bool recordsHistory = origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
            if (recordsHistory) {
                before = captureClipFragment(trackIndex, clipId);
                if (!before)
                    return false;
            }

            auto replacement = std::make_unique<MidiClipSourceNode>(
                existing->instanceId(),
                std::move(umpEvents),
                std::move(umpTickTimestamps),
                clip->tickResolution > 0 ? clip->tickResolution : existing->tickResolution(),
                existing->clipTempo(),
                static_cast<double>(sampleRate_),
                existing->tempoChanges(),
                existing->timeSignatureChanges());
            const int64_t newDuration = replacement->totalLength();

            // The node swap and the duration change are one edit.
            {
                ProjectDocumentTransaction transaction(project_document_events_);
                if (!targetTrack->replaceClipSourceNode(clipId, std::move(replacement)))
                    return false;
                targetTrack->clipManager().resizeClip(clipId, newDuration);
                notifyClipChanged(trackIndex, clipId, "clip-content-changed");
                notifyTimelineChanged();
            }
            return !recordsHistory || recordReplacedClip(
                trackIndex, std::move(*before), origin, "Edit MIDI clip content");
        }

        std::optional<ProjectClipFragment> captureClipFragment(
            int32_t trackIndex,
            int32_t clipId) const override {
            // Extensions contribute state here, and at least one of them --
            // ARA -- cannot legally archive while the document is being edited.
            // Refusing at this boundary reports the mistake at the real call
            // site rather than as a missing slot discovered much later.
            if (project_document_events_.inTransaction()) {
                std::cerr << "Error: captureClipFragment must not be called inside a document "
                             "transaction; capture first, then mutate." << std::endl;
                return std::nullopt;
            }

            const auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack)
                return std::nullopt;
            const auto* clip = targetTrack->clipManager().getClip(clipId);
            if (!clip)
                return std::nullopt;

            ProjectClipFragment fragment;
            fragment.clip = *clip;
            if (clip->clipType == ClipType::Midi) {
                // Audio clips are rebuilt from their file, but MIDI content is
                // authored and exists nowhere else, so it must be copied out.
                auto sourceNode = const_cast<TimelineTrack*>(targetTrack)
                    ->getSourceNode(clip->sourceNodeInstanceId);
                if (auto midi = std::dynamic_pointer_cast<MidiClipSourceNode>(sourceNode)) {
                    fragment.umpEvents = midi->umpEvents();
                    fragment.umpTickTimestamps = midi->eventTimestampsTicks();
                    fragment.tempoChanges = midi->tempoChanges();
                    fragment.timeSignatureChanges = midi->timeSignatureChanges();
                }
            }

            // Collect state owned outside the document, such as a plug-in's
            // opaque per-clip state, so the fragment is self-contained.
            for (auto* extension : projectSerializationExtensionsSnapshot()) {
                std::vector<uint8_t> state;
                std::string extensionError;
                if (!extension->captureClipFragmentState(clip->referenceId, state, extensionError)) {
                    // Continuing would hand back a fragment that looks complete
                    // but has lost state the user cannot see and cannot
                    // recover. Failing the capture keeps the operation honest.
                    std::cerr << "Error: Extension " << extension->extensionId()
                              << " could not capture its state for clip "
                              << clip->referenceId << ": " << extensionError
                              << ". Capture abandoned rather than returning an incomplete fragment."
                              << std::endl;
                    return std::nullopt;
                }
                if (!state.empty())
                    fragment.extensionState[std::string(extension->extensionId())] = std::move(state);
            }
            return fragment;
        }

        ClipAddResult attachClipFragment(
            int32_t trackIndex,
            const ProjectClipFragment& fragment,
            ProjectObjectIdPolicy idPolicy) override {
            ClipAddResult result;
            auto* targetTrack = resolveTrack(trackIndex);
            if (!targetTrack) {
                result.error = "Invalid track index";
                return result;
            }

            // Recreating the clip and reapplying its metadata is one edit.
            ProjectDocumentTransaction transaction(project_document_events_);

            // Restore reuses the captured identity; Mint leaves the staging
            // slot empty so the usual allocation happens.
            pending_clip_reference_id_ = idPolicy == ProjectObjectIdPolicy::Restore
                ? fragment.clip.referenceId
                : std::string{};

            const auto& source = fragment.clip;
            if (fragment.isMidi()) {
                result = addMidiClipToTimelineTrack(
                    *targetTrack,
                    source.position,
                    source.filepath,
                    fragment.umpEvents,
                    fragment.umpTickTimestamps,
                    source.tickResolution,
                    source.clipTempo,
                    fragment.tempoChanges,
                    fragment.timeSignatureChanges,
                    source.name,
                    source.nrpnToParameterMapping,
                    source.needsFileSave,
                    idPolicy == ProjectObjectIdPolicy::Restore ? source.clipId : -1);
            } else {
                std::unique_ptr<AudioFileReader> reader;
                if (source.filepath.empty()) {
                    reader = std::make_unique<SilentAudioFileReader>(
                        static_cast<uint64_t>(std::max<int64_t>(1, source.durationSamples)),
                        std::max<uint32_t>(1, targetTrack->channelCount()),
                        static_cast<uint32_t>(std::max(1, sampleRate_)));
                } else {
                    reader = createAudioFileReaderFromPath(source.filepath);
                }
                if (!reader) {
                    pending_clip_reference_id_.clear();
                    result.error = "Could not reopen the audio file for the clip";
                    return result;
                }
                result = addAudioClipToTrack(
                    *targetTrack,
                    source.position,
                    std::move(reader),
                    source.filepath,
                    source.markers,
                    source.audioWarps,
                    idPolicy == ProjectObjectIdPolicy::Restore ? source.clipId : -1);
            }

            // Staging is consumed by a successful add; clear it so a failed one
            // cannot hand the captured identity to an unrelated later clip.
            pending_clip_reference_id_.clear();
            if (!result.success)
                return result;

            // Properties the add paths do not take. Applied through the
            // mutators so each is a document change like any other; the
            // surrounding transaction keeps them one batch.
            auto& clips = targetTrack->clipManager();
            clips.setClipGain(result.clipId, source.gain);
            clips.setClipMuted(result.clipId, source.muted);
            if (!clips.setClipAnchor(result.clipId, source.timeReference(sampleRate_), sampleRate_)) {
                removeClipRaw(*targetTrack, result.clipId);
                result.success = false;
                result.error = "Could not restore the clip's timeline anchor";
                return result;
            }
            setClipEnabled(
                trackIndex, result.clipId, source.enabled,
                ProjectMutationOrigin::Internal);
            resizeClip(
                trackIndex, result.clipId, source.durationSamples,
                ProjectMutationOrigin::Internal);
            if (fragment.isMidi() && !source.markers.empty())
                setClipMarkers(
                    trackIndex, result.clipId, source.markers,
                    ProjectMutationOrigin::Internal);

            // Hand each extension its own slot back, addressed by the identity
            // the clip has now: the captured one when restoring, a freshly
            // minted one when pasting.
            if (const auto* attachedClip = clips.getClip(result.clipId)) {
                for (auto* extension : projectSerializationExtensionsSnapshot()) {
                    const auto it = fragment.extensionState.find(std::string(extension->extensionId()));
                    static const std::vector<uint8_t> kNoState{};
                    const auto& state = it == fragment.extensionState.end() ? kNoState : it->second;
                    std::string extensionError;
                    if (!extension->restoreClipFragmentState(attachedClip->referenceId, state, extensionError)) {
                        const auto failedClipReferenceId = attachedClip->referenceId;
                        removeClipRaw(*targetTrack, result.clipId);
                        result.success = false;
                        result.error = std::format(
                            "Extension {} failed to restore state for clip {}: {}",
                            extension->extensionId(),
                            failedClipReferenceId,
                            extensionError);
                        return result;
                    }
                }
            }
            return result;
        }

        bool appendMidiEventsToClip(int32_t trackIndex, int32_t clipId,
            std::vector<uapmd_ump_t> words, std::vector<uint64_t> ticks) override {
            if (words.empty() || words.size() != ticks.size())
                return false;
            TimelineTrack* track = trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())
                ? timeline_tracks_[static_cast<size_t>(trackIndex)].get() : nullptr;
            auto* clip = track ? track->clipManager().getClip(clipId) : nullptr;
            if (!clip || clip->clipType != ClipType::Midi)
                return false;
            auto source = track->getSourceNode(clip->sourceNodeInstanceId);
            auto midi = std::dynamic_pointer_cast<MidiClipSourceNode>(source);
            if (!midi)
                return false;
            struct TimedUmp {
                uint64_t tick{};
                std::vector<uapmd_ump_t> words;
                bool recorded{};
            };
            std::vector<TimedUmp> events;
            const auto appendEvents = [&events](const std::vector<uapmd_ump_t>& sourceWords,
                                                const std::vector<uint64_t>& sourceTicks,
                                                bool recorded) {
                for (size_t offset = 0; offset < sourceWords.size();) {
                    const auto wordCount = std::max<size_t>(1,
                        umppi::umpSizeInInts(static_cast<uint8_t>(sourceWords[offset] >> 28)));
                    if (offset + wordCount > sourceWords.size() || offset >= sourceTicks.size())
                        return false;
                    events.push_back({sourceTicks[offset],
                        std::vector<uapmd_ump_t>(sourceWords.begin() + static_cast<std::ptrdiff_t>(offset),
                                                 sourceWords.begin() + static_cast<std::ptrdiff_t>(offset + wordCount)),
                        recorded});
                    offset += wordCount;
                }
                return true;
            };
            if (!appendEvents(midi->umpEvents(), midi->eventTimestampsTicks(), false) ||
                !appendEvents(words, ticks, true))
                return false;
            // At an identical tick preserve existing clip data before newly
            // recorded input; stable_sort retains each stream's event order.
            std::stable_sort(events.begin(), events.end(), [](const TimedUmp& a, const TimedUmp& b) {
                if (a.tick != b.tick)
                    return a.tick < b.tick;
                return !a.recorded && b.recorded;
            });
            std::vector<uapmd_ump_t> allWords;
            std::vector<uint64_t> allTicks;
            for (const auto& event : events) {
                allWords.insert(allWords.end(), event.words.begin(), event.words.end());
                allTicks.insert(allTicks.end(), event.words.size(), event.tick);
            }
            return replaceMidiClipContent(
                trackIndex,
                clipId,
                std::move(allWords),
                std::move(allTicks),
                ProjectMutationOrigin::User);
        }

        void loadProject(const std::filesystem::path& projectFile, ProjectLoadCallback callback) override {
            if (engine_.frozenTrackManager().hasBusyTrack()) {
                callback({
                    false,
                    "Unfreeze the busy track before loading a project"});
                return;
            }
            if (undo_engine_.state().busy) {
                callback({false, "Wait for the pending undo operation before loading a project"});
                return;
            }
            if (projectFile.empty()) {
                callback({false, "Project path is empty"});
                return;
            }

            auto project = UapmdProjectDataReader::read(projectFile);
            if (!project) {
                callback({false, "Failed to parse project file"});
                return;
            }

            auto projectDir = projectFile.parent_path();

            // Parsing succeeded, so everything below replaces the current
            // project. Never replay old operations against the new object set.
            // A failed load leaves the partial replacement dirty; successful
            // completion establishes a clean history root below.
            undo_engine_.clear(false);

            ProjectDocumentEvent closingEvent(ProjectDocumentEventKind::ProjectClosing, "project-closing");
            closingEvent.setProjectId(projectFile.string())
                .setFullResyncRecommended(true)
                .setDetail("source.file", projectFile.string());
            emitProjectDocumentEvent(std::move(closingEvent));

            suppress_timeline_notification_ = true;
            suppress_project_document_events_ = true;

            timeline_.isPlaying = false;
            timeline_.playheadPosition = TimelinePosition{};
            timeline_.loopEnabled = false;
            timeline_.projectTickResolution = 0;
            next_timeline_track_reference_ = 1;
            master_timeline_track_ = std::make_shared<TimelineTrack>(
                std::string("master_track"),
                0,
                sampleRate_ > 0 ? static_cast<double>(sampleRate_) : 48000.0,
                bufferSizeInFrames_);

            // Clear all existing tracks via engine (which calls onTrackRemoved for each)
            // NOTE: SequencerEngine::tracks() returns a transient snapshot, so refresh it
            // every iteration to ensure we see the latest state.
            while (true) {
                auto& snapshot = engine_.tracks();
                if (snapshot.empty())
                    break;
                engine_.removeTrack(static_cast<uapmd_track_index_t>(snapshot.size() - 1));
            }

            // Sentinel starts at 1 (representing the synchronous setup phase).
            // Each addPluginToTrack call increments it; each callback decrements it.
            // Releasing the sentinel at the end of setup triggers finish when no plugins are pending.
            auto pending_plugins = std::make_shared<std::atomic<int>>(1);
            auto finish_holder = std::make_shared<std::function<void()>>();
            using PluginLoadStep = std::function<void(std::function<void()>)>;
            auto plugin_load_steps = std::make_shared<std::vector<PluginLoadStep>>();

            struct LoadedClipRef {
                TimelineTrack* track{nullptr};
                int32_t clipId{-1};
                std::string clipReferenceId;
            };
            std::unordered_map<UapmdProjectClipData*, LoadedClipRef> loadedClipRefs;

            // FIXME: we might have to reconsider how we adapt plugin instances instantiated here to the graph.
            //  Currently, `UapmdPluginGraphBuilder::build()` is practically no-op, but the project loads.
            //  It is because it is already added in a linear manner.
            //  But that may not be the right thing depending on the graphs (such as, full DAG).
            auto loadPluginsForTrack = [this, &projectDir, plugin_load_steps](UapmdProjectTrackData* projectTrack, int32_t trackIndex) {
                if (!projectTrack)
                    return;
                auto* graphData = projectTrack->graph();
                if (!graphData)
                    return;
                auto* provider = audio_graph_provider_registry_.get(graphData->graphType());
                auto externalGraphFile = graphData->externalFile();
                if (!externalGraphFile.empty()) {
                    if (provider) {
                        auto resolvedGraphFile = makeAbsolutePath(projectDir, externalGraphFile);
                        std::vector<uint8_t> graphBytes;
                        std::ifstream graphInput(resolvedGraphFile, std::ios::binary);
                        if (graphInput)
                            graphBytes.assign(std::istreambuf_iterator<char>(graphInput), {});
                        auto loadedGraphData = graphBytes.empty()
                            ? std::unique_ptr<UapmdProjectPluginGraphData>{}
                            : loadSerializedProjectGraph(*provider, *graphData, graphBytes);
                        if (!loadedGraphData) {
                            std::cerr << "Warning: Failed to load external graph file "
                                      << resolvedGraphFile << ". Falling back to embedded graph data." << std::endl;
                        } else {
                            projectTrack->graph(std::move(loadedGraphData));
                            graphData = projectTrack->graph();
                        }
                    }
                }
                if (!provider)
                    return;
                auto* pluginHost = engine_.pluginHost();
                std::vector<remidy::PluginCatalogEntry> catalogEntries;
                bool catalogLoaded = false;
                auto ensureCatalogLoaded = [&]() -> std::vector<remidy::PluginCatalogEntry>& {
                    if (!catalogLoaded && pluginHost) {
                        catalogEntries = pluginHost->pluginCatalogEntries();
                        catalogLoaded = true;
                    }
                    return catalogEntries;
                };
                auto catalogHasPlugin = [&](const std::string& format, const std::string& pluginId) -> bool {
                    if (!pluginHost)
                        return true; // Cannot verify without host; assume valid
                    if (pluginId.empty())
                        return false;
                    auto& entries = ensureCatalogLoaded();
                    return std::any_of(entries.begin(), entries.end(),
                        [&](remidy::PluginCatalogEntry& entry) {
                            return entry.format() == format && entry.pluginId() == pluginId;
                        });
                };
                auto catalogFindByName = [&](const std::string& format, const std::string& displayName) -> std::string {
                    if (!pluginHost || displayName.empty())
                        return {};
                    auto& entries = ensureCatalogLoaded();
                    std::string resolvedId;
                    for (auto& entry : entries) {
                        if (entry.format() == format && entry.displayName() == displayName) {
                            if (resolvedId.empty())
                                resolvedId = entry.pluginId();
                            else if (resolvedId != entry.pluginId())
                                return {}; // Ambiguous
                        }
                    }
                    return resolvedId;
                };

                for (const auto& plugin : provider->getPluginNodeDataListFrom(graphData)) {
                    if (plugin.format.empty()) {
                        std::cerr << "Warning: Skipping plugin node with missing format while loading project." << std::endl;
                        continue;
                    }
                    std::string format = plugin.format;
                    std::string pluginId = plugin.plugin_id;
                    std::string stateFile = plugin.state_file;
                    // Restore the node under the identity it was saved with, so
                    // that ARA archives and saved graph connections reconnect.
                    std::string nodeId = plugin.node_id;
                    const std::string pluginName = plugin.display_name;
                    const int32_t groupIndex = plugin.group_index;

                    if (pluginId.empty()) {
                        auto fallbackId = catalogFindByName(format, pluginName);
                        if (!fallbackId.empty()) {
                            std::cerr << "Info: Plugin \"" << pluginName
                                      << "\" missing ID; resolved using catalog entry ID " << fallbackId << "." << std::endl;
                            pluginId = fallbackId;
                        }
                    } else if (!catalogHasPlugin(format, pluginId)) {
                        auto fallbackId = catalogFindByName(format, pluginName);
                        if (!fallbackId.empty()) {
                            std::cerr << "Info: Plugin \"" << (pluginName.empty() ? pluginId : pluginName)
                                      << "\" not found by ID; substituting catalog entry ID " << fallbackId << "." << std::endl;
                            pluginId = fallbackId;
                        }
                    }

                    if (pluginId.empty()) {
                        std::cerr << "Warning: Unable to resolve plugin entry (format=" << format
                                  << ", name=" << pluginName << "). Plugin will be skipped." << std::endl;
                        continue;
                    }

                    std::filesystem::path resolvedState;
                    if (!stateFile.empty())
                        resolvedState = makeAbsolutePath(projectDir, stateFile);

                    const std::string pluginLabel = pluginName.empty() ? pluginId : pluginName;

                    plugin_load_steps->push_back(
                        [this, trackIndex, format = std::move(format), pluginId = std::move(pluginId),
                         resolvedState, groupIndex, pluginLabel, nodeId = std::move(nodeId)](std::function<void()> done) mutable {
                            engine_.addPluginToTrack(trackIndex, format, pluginId,
                                [this, resolvedState, groupIndex, pluginLabel, pluginId, format, done = std::move(done)](int32_t instanceId, int32_t, std::string error) mutable {
                            auto finishPlugin = [done]() mutable {
                                if (done)
                                    done();
                            };

                            if (!error.empty()) {
                                std::cerr << "Warning: Failed to instantiate plugin " << pluginLabel
                                          << " (" << format << ", ID=" << pluginId << "): " << error << std::endl;
                            } else if (instanceId >= 0) {
                                // Restore saved group assignment (overrides auto-assigned group).
                                if (groupIndex >= 0 && groupIndex <= 15)
                                    engine_.setInstanceGroup(instanceId, static_cast<uint8_t>(groupIndex));

                                if (!resolvedState.empty()) {
                                    auto* instance = engine_.getPluginInstance(instanceId);
                                    if (instance) {
                                        std::ifstream f(resolvedState, std::ios::binary);
                                        if (f) {
                                            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
                                            instance->loadStateSync(data);
                                        } else {
                                            std::cerr << "Warning: Failed to open state file for plugin "
                                                      << pluginLabel << ": " << resolvedState << std::endl;
                                        }
                                    } else {
                                        std::cerr << "Warning: Failed to get plugin instance " << instanceId
                                                  << " while restoring state for " << pluginLabel << std::endl;
                                    }
                                }
                            }
                            finishPlugin();
                        },
                        std::move(nodeId));
                    });
                }
            };

            auto* masterProjectTrack = project->masterTrack();
            std::vector<ClipMarker> masterTrackMarkers;
            if (masterProjectTrack)
                masterTrackMarkers = masterProjectTrack->markers();
            const bool hasExplicitMasterTrackClips = masterProjectTrack && !masterProjectTrack->clips().empty();

            std::string earlyError;
            auto& tracks = project->tracks();

            for (size_t i = 0; i < tracks.size() && earlyError.empty(); ++i) {
                // Restore the track under the identity it was saved with, so
                // that anything keyed by it (ARA persistent IDs, frozen track
                // state) still resolves after a reload.
                pending_track_reference_id_ = tracks[i]->referenceId();
                int32_t trackIndex = engine_.addEmptyTrack();
                pending_track_reference_id_.clear();
                if (trackIndex < 0) {
                    earlyError = "Failed to create track";
                    break;
                }

                loadPluginsForTrack(tracks[i], trackIndex);

                for (auto& clip : tracks[i]->clips()) {
                    if (!clip)
                        continue;

                    // Staged per clip, so a clip that fails to load cannot
                    // leak its identity onto the next one.
                    pending_clip_reference_id_ = clip->referenceId();

                    auto absoluteSamples = static_cast<int64_t>(clip->absolutePositionInSamples());
                    TimelinePosition position;
                    position.samples = absoluteSamples;

                    const auto clipFile = clip->file();
                    const auto clipType = clip->clipType();
                    std::filesystem::path resolvedPath = clipFile;
                    if (!resolvedPath.empty())
                        resolvedPath = makeAbsolutePath(projectDir, resolvedPath);

                    if (clipType == "midi") {
                        if (resolvedPath.empty()) {
                            earlyError = "MIDI clip is missing file path";
                            break;
                        }
                        auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
                        if (!clipInfo.success) {
                            earlyError = clipInfo.error.empty() ? "Failed to parse MIDI clip" : clipInfo.error;
                            break;
                        }
                        auto separated = MidiClipReader::separateMasterTrackEvents(std::move(clipInfo));
                        auto& musicalClip = separated.musicalClip;
                        auto& masterClip = separated.masterTrackClip;
                        if (separated.hasMusicalClip()) {
                            double clipTempo = musicalClip.tempo_changes.empty() ? 120.0 : musicalClip.tempo_changes.front().bpm;
                            if (clipTempo <= 0.0) clipTempo = 120.0;
                            auto loadResult = addMidiClipToTrack(
                                trackIndex, position,
                                std::move(musicalClip.ump_data),
                                std::move(musicalClip.ump_tick_timestamps),
                                musicalClip.tick_resolution,
                                clipTempo,
                                std::move(musicalClip.tempo_changes),
                                std::move(musicalClip.time_signature_changes),
                                resolvedPath.filename().string(),
                                clip->nrpnToParameterMapping(),
                                separated.hasMasterTrackClip(),
                                ProjectMutationOrigin::Load);
                            if (!loadResult.success) {
                                earlyError = loadResult.error.empty() ? "Failed to load MIDI clip" : loadResult.error;
                                break;
                            }
                            if (auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId)) {
                                loadedClip->markers = clip->markers();
                                loadedClip->audioWarps = clip->audioWarps();
                            }
                            auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId);
                            loadedClipRefs[clip.get()] = LoadedClipRef{
                                timeline_tracks_[static_cast<size_t>(trackIndex)].get(),
                                loadResult.clipId,
                                loadedClip ? loadedClip->referenceId : std::string{}};
                        }
                        if (!hasExplicitMasterTrackClips && separated.hasMasterTrackClip()) {
                            auto masterLoadResult = addMasterMidiClip(
                                position,
                                {},
                                {},
                                masterClip.tick_resolution,
                                masterClip.tempo,
                                std::move(masterClip.tempo_changes),
                                std::move(masterClip.time_signature_changes),
                                std::format("{} Meta", resolvedPath.filename().string()),
                                false,
                                "",
                                ProjectMutationOrigin::Load);
                            if (!masterLoadResult.success) {
                                earlyError = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                                break;
                            }
                            if (auto refIt = loadedClipRefs.find(clip.get()); refIt != loadedClipRefs.end() && !refIt->second.clipReferenceId.empty())
                                master_timeline_track_->clipManager().setClipAnchor(
                                    masterLoadResult.clipId,
                                    TimeReference::fromContainerStart(refIt->second.clipReferenceId, 0.0),
                                    sampleRate_);
                        }
                    } else {
                        std::unique_ptr<AudioFileReader> reader;
                        std::string filepath;
                        if (resolvedPath.empty()) {
                            const int64_t durationSamples = std::max<int64_t>(
                                1,
                                clip->durationSamples() > 0
                                    ? clip->durationSamples()
                                    : static_cast<int64_t>(sampleRate_));
                            const uint32_t channelCount = std::max<uint32_t>(
                                1,
                                timeline_tracks_[static_cast<size_t>(trackIndex)]->channelCount());
                            reader = std::make_unique<SilentAudioFileReader>(
                                static_cast<uint64_t>(durationSamples),
                                channelCount,
                                static_cast<uint32_t>(sampleRate_));
                        } else {
                            reader = createAudioFileReaderFromPath(resolvedPath.string());
                            if (!reader) {
                                earlyError = std::format("Failed to open audio clip {}", resolvedPath.string());
                                break;
                            }
                            filepath = resolvedPath.string();
                        }
                        auto loadResult = addAudioClipToTrack(
                            *timeline_tracks_[static_cast<size_t>(trackIndex)],
                            position,
                            std::move(reader),
                            filepath,
                            clip->markers(),
                            clip->audioWarps());
                        if (!loadResult.success) {
                            earlyError = loadResult.error.empty() ? "Failed to load audio clip" : loadResult.error;
                            break;
                        }
                        auto* loadedClip = timeline_tracks_[static_cast<size_t>(trackIndex)]->clipManager().getClip(loadResult.clipId);
                        loadedClipRefs[clip.get()] = LoadedClipRef{
                            timeline_tracks_[static_cast<size_t>(trackIndex)].get(),
                            loadResult.clipId,
                            loadedClip ? loadedClip->referenceId : std::string{}};
                    }
                }
            }

            // Load master track clips (tempo/time-signature map)
            if (earlyError.empty() && masterProjectTrack) {
                loadPluginsForTrack(masterProjectTrack, kMasterTrackIndex);
                for (auto& clip : masterProjectTrack->clips()) {
                    if (!clip || clip->clipType() != "midi")
                        continue;
                    pending_clip_reference_id_ = clip->referenceId();
                    auto resolvedPath = makeAbsolutePath(projectDir, clip->file());
                    if (resolvedPath.empty())
                        continue;
                    auto clipInfo = MidiClipReader::readAnyFormat(resolvedPath);
                    if (!clipInfo.success)
                        continue;
                    double clipTempo = clipInfo.tempo_changes.empty() ? 120.0 : clipInfo.tempo_changes.front().bpm;
                    if (clipTempo <= 0.0) clipTempo = 120.0;
                    if (!clipInfo.tempo_changes.empty())
                        timeline_.tempo = clipTempo;
                    TimelinePosition pos;
                    pos.samples = static_cast<int64_t>(clip->absolutePositionInSamples());
                    auto masterLoadResult = addMasterMidiClip(
                        pos,
                        std::move(clipInfo.ump_data),
                        std::move(clipInfo.ump_tick_timestamps),
                        clipInfo.tick_resolution,
                        clipTempo,
                        std::move(clipInfo.tempo_changes),
                        std::move(clipInfo.time_signature_changes),
                        resolvedPath.filename().string(),
                        false,
                        resolvedPath.string(),
                        ProjectMutationOrigin::Load);
                    if (!masterLoadResult.success) {
                        earlyError = masterLoadResult.error.empty() ? "Failed to load master track clip" : masterLoadResult.error;
                        break;
                    }
                    auto* loadedClip = master_timeline_track_->clipManager().getClip(masterLoadResult.clipId);
                    if (loadedClip) {
                        loadedClip->markers = clip->markers();
                        loadedClip->audioWarps = clip->audioWarps();
                    }
                    loadedClipRefs[clip.get()] = LoadedClipRef{
                        master_timeline_track_.get(),
                        masterLoadResult.clipId,
                        loadedClip ? loadedClip->referenceId : std::string{}};
                }
            }

            if (earlyError.empty()) {
                auto applyAnchorToLoadedClip = [this, &loadedClipRefs](UapmdProjectClipData* projectClip) {
                    if (!projectClip)
                        return;
                    auto loadedIt = loadedClipRefs.find(projectClip);
                    if (loadedIt == loadedClipRefs.end())
                        return;

                    auto pos = projectClip->position();
                    auto* targetTrack = loadedIt->second.track;
                    if (!targetTrack)
                        return;

                    TimeReference anchor = TimeReference::fromContainerStart();
                    if (auto* anchorClip = dynamic_cast<UapmdProjectClipData*>(pos.anchor)) {
                        auto anchorIt = loadedClipRefs.find(anchorClip);
                        if (anchorIt != loadedClipRefs.end())
                            anchor.referenceId = anchorIt->second.clipReferenceId;
                    }
                    anchor.type = pos.origin == UapmdAnchorOrigin::End
                        ? TimeReferenceType::ContainerEnd
                        : TimeReferenceType::ContainerStart;
                    anchor.offset = TimelinePosition(static_cast<int64_t>(pos.samples)).toSeconds(sampleRate_);
                    targetTrack->clipManager().setClipAnchor(
                        loadedIt->second.clipId,
                        anchor,
                        sampleRate_);
                    targetTrack->clipManager().setClipPosition(
                        loadedIt->second.clipId,
                        TimelinePosition(static_cast<int64_t>(projectClip->absolutePositionInSamples())));
                };

                for (auto* projectTrack : tracks)
                    for (auto& clip : projectTrack->clips())
                        applyAnchorToLoadedClip(clip.get());
                if (masterProjectTrack)
                    for (auto& clip : masterProjectTrack->clips())
                        applyAnchorToLoadedClip(clip.get());
            }

            // Set finish_holder before releasing the sentinel.
            // finish_holder is always set before any plugin callback can observe pending==0.
            if (!earlyError.empty()) {
                suppress_timeline_notification_ = false;
                suppress_project_document_events_ = false;
                *finish_holder = [callback = std::move(callback), earlyError = std::move(earlyError)]() mutable {
                    callback({false, std::move(earlyError)});
                };
            } else {
                auto sharedProject = std::shared_ptr<UapmdProjectData>(std::move(project));
                auto finishLoadedProject = [this, projectFile, projectDir,
                                            sharedProject = std::move(sharedProject),
                                            masterTrackMarkers = std::move(masterTrackMarkers),
                                            callback = std::move(callback)]() mutable {
                    suppress_timeline_notification_ = false;
                    suppress_project_document_events_ = false;

                    auto applyGraphConnections = [this](UapmdProjectTrackData* projectTrack, SequencerTrack* sequencerTrack) {
                        if (!projectTrack || !sequencerTrack || !projectTrack->graph())
                            return;
                        materializeProjectGraph(projectTrack, sequencerTrack, engine_.umpBufferSizeInBytes());
                    };

                    auto& tracks = sharedProject->tracks();
                    auto* masterProjectTrack = sharedProject->masterTrack();
                    for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i)
                        applyGraphConnections(tracks[i], engine_.tracks()[i]);
                    if (masterProjectTrack)
                        applyGraphConnections(masterProjectTrack, engine_.masterTrack());

                    std::string projectDataError;
                    if (!loadProjectDataExtensions(*sharedProject, projectDataError)) {
                        callback({false, std::move(projectDataError)});
                        return;
                    }

                    for (size_t i = 0; i < tracks.size() && i < engine_.tracks().size(); ++i) {
                        if (tracks[i] && engine_.tracks()[i]) {
                            engine_.tracks()[i]->trackGain(tracks[i]->volume());
                            engine_.tracks()[i]->muted(tracks[i]->muted());
                            engine_.tracks()[i]->solo(tracks[i]->solo());
                        }
                    }
                    if (masterProjectTrack && engine_.masterTrack()) {
                        engine_.masterTrack()->trackGain(masterProjectTrack->volume());
                    }

                    ProjectDocumentEvent loadedEvent(ProjectDocumentEventKind::ProjectLoaded, "project-loaded");
                    loadedEvent.setProjectId(projectFile.string())
                        .setFullResyncRecommended(true)
                        .setDetail("source.file", projectFile.string());
                    emitProjectDocumentEvent(std::move(loadedEvent));
                    emitMasterTrackChanged("master-track-content-changed");
                    std::string extensionError;
                    if (!loadProjectExtensionData(projectFile, projectDir, extensionError)) {
                        callback({false, std::move(extensionError)});
                        return;
                    }
                    undo_engine_.clear(true);
                    notifyTimelineChanged();
                    callback({true, {}, std::move(masterTrackMarkers)});
                };
                *finish_holder = [finish = std::move(finishLoadedProject)]() mutable {
                    if (remidy::EventLoop::runningOnMainThread())
                        finish();
                    else
                        remidy::EventLoop::enqueueTaskOnMainThread(std::move(finish));
                };
            }

            if (earlyError.empty() && !plugin_load_steps->empty()) {
                pending_plugins->fetch_add(1, std::memory_order_relaxed);
                auto next_index = std::make_shared<size_t>(0);
                auto run_next = std::make_shared<std::function<void()>>();
                *run_next = [plugin_load_steps, next_index, run_next, pending_plugins, finish_holder]() mutable {
                    if (*next_index >= plugin_load_steps->size()) {
                        if (pending_plugins->fetch_sub(1, std::memory_order_acq_rel) == 1)
                            (*finish_holder)();
                        return;
                    }
                    auto step = (*plugin_load_steps)[(*next_index)++];
                    step([run_next]() mutable {
                        (*run_next)();
                    });
                };
                (*run_next)();
            }

            // Release the sentinel; if it reaches 0 (no plugins pending), fire finish now.
            if (pending_plugins->fetch_sub(1, std::memory_order_acq_rel) == 1)
                (*finish_holder)();
        }

        MasterTrackSnapshot buildMasterTrackSnapshot() override {
            MasterTrackSnapshot snapshot;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            auto appendTrackMeta = [&snapshot, sr](const std::shared_ptr<TimelineTrack>& track) {
                if (!track)
                    return;
                auto clips = track->clipManager().getAllClips();
                std::sort(clips.begin(), clips.end(), [](const ClipData& a, const ClipData& b) {
                    return a.clipId < b.clipId;
                });

                for (const auto& clip : clips) {
                    if (clip.clipType != ClipType::Midi)
                        continue;
                    auto sourceNode = track->getSourceNode(clip.sourceNodeInstanceId);
                    auto* midiNode = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
                    if (!midiNode)
                        continue;
                    appendMidiNodeMetaToSnapshot(snapshot, clip, *midiNode, sr);
                }
            };

            // Regular tracks can never carry meaningful tempo/time-signature data of their own
            // (see MidiClipReader::stripToFlatTempo / TrackImporter::importMidiFile) -- the
            // master track is always the sole source, so no fallback search is needed here.
            appendTrackMeta(master_timeline_track_);

            std::stable_sort(snapshot.tempoPoints.begin(), snapshot.tempoPoints.end(),
                [](const MasterTrackSnapshot::TempoPoint& a, const MasterTrackSnapshot::TempoPoint& b) {
                    return a.timeSeconds < b.timeSeconds;
                });
            std::stable_sort(snapshot.timeSignaturePoints.begin(), snapshot.timeSignaturePoints.end(),
                [](const MasterTrackSnapshot::TimeSignaturePoint& a, const MasterTrackSnapshot::TimeSignaturePoint& b) {
                    return a.timeSeconds < b.timeSeconds;
                });

            return snapshot;
        }

        ContentBounds calculateTrackContentBounds(int32_t trackIndex) const override {
            ContentBounds bounds;
            if (trackIndex < 0 ||
                static_cast<size_t>(trackIndex) >= timeline_tracks_.size())
                return bounds;

            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            const auto& trackPtr =
                timeline_tracks_[static_cast<size_t>(trackIndex)];
            if (!trackPtr)
                return bounds;

            const auto snapshot = trackPtr->clipManager().getSnapshotRT();
            if (!snapshot)
                return bounds;

            for (const auto& clip : snapshot->clips) {
                const auto absolute =
                    clip.getAbsolutePosition(snapshot->clipReferenceMap);
                const int64_t startSample = absolute.samples;
                const int64_t durationSamples =
                    std::max<int64_t>(0, clip.durationSamples);
                const int64_t endSample =
                    startSample > 0 &&
                        durationSamples >
                            std::numeric_limits<int64_t>::max() - startSample
                    ? std::numeric_limits<int64_t>::max()
                    : startSample + durationSamples;

                if (!bounds.hasContent || startSample < bounds.firstSample) {
                    bounds.firstSample = startSample;
                    bounds.firstSeconds = static_cast<double>(startSample) / sr;
                }
                if (!bounds.hasContent || endSample > bounds.lastSample) {
                    bounds.lastSample = endSample;
                    bounds.lastSeconds = static_cast<double>(endSample) / sr;
                }
                bounds.hasContent = true;
            }
            return bounds;
        }

        ContentBounds calculateContentBounds() const override {
            ContentBounds bounds;
            const double sr = std::max(1.0, static_cast<double>(sampleRate_));
            for (size_t trackIndex = 0;
                 trackIndex < timeline_tracks_.size();
                 ++trackIndex) {
                const auto trackBounds =
                    calculateTrackContentBounds(
                        static_cast<int32_t>(trackIndex));
                if (!trackBounds.hasContent)
                    continue;
                if (!bounds.hasContent ||
                    trackBounds.firstSample < bounds.firstSample)
                    bounds.firstSample = trackBounds.firstSample;
                if (!bounds.hasContent ||
                    trackBounds.lastSample > bounds.lastSample)
                    bounds.lastSample = trackBounds.lastSample;
                bounds.hasContent = true;
            }
            if (bounds.hasContent) {
                bounds.firstSeconds =
                    static_cast<double>(bounds.firstSample) / sr;
                bounds.lastSeconds =
                    static_cast<double>(bounds.lastSample) / sr;
            }
            return bounds;
        }

        void setTimelineChangedCallback(std::function<void()> cb) override {
            timeline_changed_callback_ = std::move(cb);
        }

        std::optional<std::vector<MidiNotePreview>> getMidiClipNotes(int32_t trackIndex, int32_t clipId) const override {
            TimelineTrack* track = nullptr;
            if (trackIndex == kMasterTrackIndex) {
                track = master_timeline_track_.get();
            } else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
                track = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            }
            if (!track) return std::nullopt;

            const ClipData* clip = track->clipManager().getClip(clipId);
            if (!clip || clip->clipType != ClipType::Midi) return std::nullopt;

            auto sourceNode = const_cast<TimelineTrack*>(track)->getSourceNode(clip->sourceNodeInstanceId);
            auto* midiSource = dynamic_cast<MidiClipSourceNode*>(sourceNode.get());
            if (!midiSource) return std::nullopt;

            const auto& events    = midiSource->umpEvents();
            const auto& timestamps = midiSource->eventTimestampsSamples();
            const double safeSR   = std::max(1.0, static_cast<double>(sampleRate_));
            const double clipDur  = std::max(0.01, static_cast<double>(midiSource->totalLength()) / safeSR);
            const double kMinDur  = 1.0 / 32.0;

            std::vector<MidiNotePreview> notes;
            std::unordered_map<uint32_t, size_t> activeNoteIndices;
            activeNoteIndices.reserve(64);

            const size_t eventCount = std::min(events.size(), timestamps.size());
            size_t i = 0;
            while (i < eventCount) {
                umppi::Ump ump1(events[i]);
                const int wordCount = ump1.getSizeInInts();
                const size_t safeCount = std::min(static_cast<size_t>(wordCount), eventCount - i);
                umppi::Ump ump = (safeCount >= 2) ? umppi::Ump(events[i], events[i + 1]) : ump1;
                const double eventSeconds = static_cast<double>(timestamps[i]) / safeSR;
                const auto msgType = ump.getMessageType();

                if (msgType == umppi::MessageType::MIDI1) {
                    const uint8_t status   = ump.getStatusCode();
                    const uint8_t channel  = ump.getChannelInGroup();
                    const uint8_t group    = ump.getGroup();
                    if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                        const uint8_t noteNum  = ump.getMidi1Note();
                        const uint8_t velocity = ump.getMidi1Velocity();
                        const uint32_t key     = (static_cast<uint32_t>(group) << 12) | (static_cast<uint32_t>(channel) << 7) | noteNum;
                        if (status == umppi::MidiChannelStatus::NOTE_ON && velocity > 0) {
                            MidiNotePreview n{};
                            n.startSeconds = eventSeconds;
                            n.note     = noteNum;
                            n.velocity = velocity / 127.0f;
                            activeNoteIndices[key] = notes.size();
                            notes.push_back(n);
                        } else {
                            auto it = activeNoteIndices.find(key);
                            if (it != activeNoteIndices.end()) {
                                notes[it->second].durationSeconds = std::max(kMinDur, eventSeconds - notes[it->second].startSeconds);
                                activeNoteIndices.erase(it);
                            }
                        }
                    }
                } else if (msgType == umppi::MessageType::MIDI2) {
                    const uint8_t  status  = ump.getStatusCode();
                    const uint8_t  channel = ump.getChannelInGroup();
                    const uint8_t  group   = ump.getGroup();
                    if (status == umppi::MidiChannelStatus::NOTE_ON || status == umppi::MidiChannelStatus::NOTE_OFF) {
                        const uint8_t  noteNum = ump.getMidi2Note();
                        const uint16_t vel16   = ump.getMidi2Velocity16();
                        const uint32_t key     = (static_cast<uint32_t>(group) << 12) | (static_cast<uint32_t>(channel) << 7) | noteNum;
                        if (status == umppi::MidiChannelStatus::NOTE_ON && vel16 > 0) {
                            MidiNotePreview n{};
                            n.startSeconds = eventSeconds;
                            n.note     = noteNum;
                            n.velocity = vel16 / 65535.0f;
                            activeNoteIndices[key] = notes.size();
                            notes.push_back(n);
                        } else {
                            auto it = activeNoteIndices.find(key);
                            if (it != activeNoteIndices.end()) {
                                notes[it->second].durationSeconds = std::max(kMinDur, eventSeconds - notes[it->second].startSeconds);
                                activeNoteIndices.erase(it);
                            }
                        }
                    }
                }
                i += static_cast<size_t>(std::max(1, wordCount));
            }
            for (auto& [key, idx] : activeNoteIndices)
                notes[idx].durationSeconds = std::max(kMinDur, clipDur - notes[idx].startSeconds);

            return notes;
        }

        uint32_t maxTrackLatencyInSamples() override {
            uint32_t maxLatency = 0;
            auto& tracks = engine_.tracks();
            for (size_t i = 0; i < tracks.size(); ++i)
                maxLatency = std::max(maxLatency,
                                      engine_.trackRenderLeadInSamples(static_cast<int32_t>(i)));
            return maxLatency;
        }

        uint32_t trackRenderOffsetInSamples(int32_t trackIndex) override {
            if (trackIndex < 0)
                return 0;
            auto trackLatency = engine_.trackRenderLeadInSamples(trackIndex);
            auto masterLatency = engine_.masterTrackRenderLeadInSamples();
            return masterLatency + trackLatency;
        }

        uint32_t masterTrackRenderOffsetInSamples() override {
            return engine_.masterTrackRenderLeadInSamples();
        }

        bool trackHasLiveInput(int32_t trackIndex) override {
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= timeline_tracks_.size())
                return false;
            auto* track = timeline_tracks_[static_cast<size_t>(trackIndex)].get();
            return track ? track->hasDeviceInputSource() : false;
        }

        void processTracksAudio(AudioProcessContext& process, SequenceProcessContext& targetSequence) override {
            // Hold a snapshot reference for the duration of this callback so that
            // tracks added or removed on the UI thread cannot destroy TrackList
            // elements while we are iterating them.
            auto snapshot = std::atomic_load_explicit(
                &timeline_tracks_snapshot_, std::memory_order_acquire);

            auto wrapToLoopRange = [this](int64_t samplePosition) -> int64_t {
                if (!timeline_.loopEnabled || timeline_.loopEnd.samples <= timeline_.loopStart.samples)
                    return samplePosition;
                const auto loopLength = timeline_.loopEnd.samples - timeline_.loopStart.samples;
                if (samplePosition < timeline_.loopStart.samples)
                    return samplePosition;
                return timeline_.loopStart.samples +
                    ((samplePosition - timeline_.loopStart.samples) % loopLength);
            };

            const auto masterSnapshot = buildMasterTrackSnapshot();
            auto updateTransportMetaForPlayhead = [&masterSnapshot, this](TimelineState& state) {
                if (masterSnapshot.empty())
                    return;
                const double playheadSeconds =
                    static_cast<double>(state.playheadPosition.samples) /
                    std::max(1.0, static_cast<double>(sampleRate_));
                for (const auto& point : masterSnapshot.tempoPoints) {
                    if (point.timeSeconds <= playheadSeconds) {
                        state.tempo = point.bpm;
                    } else break;
                }
                for (const auto& point : masterSnapshot.timeSignaturePoints) {
                    if (point.timeSeconds <= playheadSeconds) {
                        state.timeSignatureNumerator = point.signature.numerator;
                        state.timeSignatureDenominator = point.signature.denominator;
                    } else break;
                }
            };

            const bool offlineRenderPlaying = engine_.offlineRendering();
            timeline_.isPlaying = engine_.isPlaybackActive();
            const auto audiblePlayheadSamples = engine_.playbackPosition();
            const auto renderPlayheadRaw = engine_.renderPlaybackPosition();
            timeline_.playheadPosition.samples = wrapToLoopRange(audiblePlayheadSamples);
            updateTransportMetaForPlayhead(timeline_);

            // Update legacy_beats
            double secondsPerBeat = 60.0 / timeline_.tempo;
            int64_t samplesPerBeat = static_cast<int64_t>(secondsPerBeat * sampleRate_);
            if (samplesPerBeat > 0) {
                timeline_.playheadPosition.legacy_beats =
                    static_cast<double>(timeline_.playheadPosition.samples) / static_cast<double>(samplesPerBeat);
            }

            // Sync to MasterContext
            TimelineState renderTransport = timeline_;
            // Offline source nodes need the same private running transport as
            // the plugins they feed. Keep the shared application timeline
            // stopped, but allow audio sources and MIDI clips to produce the
            // content being frozen.
            renderTransport.isPlaying =
                timeline_.isPlaying || offlineRenderPlaying;
            renderTransport.playheadPosition.samples = wrapToLoopRange(
                (timeline_.isPlaying || offlineRenderPlaying ||
                 renderPlayheadRaw != audiblePlayheadSamples) ?
                    renderPlayheadRaw : audiblePlayheadSamples
            );
            updateTransportMetaForPlayhead(renderTransport);
            const double renderSecondsPerBeat = 60.0 / renderTransport.tempo;
            const int64_t renderSamplesPerBeat = static_cast<int64_t>(renderSecondsPerBeat * sampleRate_);
            if (renderSamplesPerBeat > 0) {
                renderTransport.playheadPosition.legacy_beats =
                    static_cast<double>(renderTransport.playheadPosition.samples) /
                    static_cast<double>(renderSamplesPerBeat);
            }

            auto& masterCtx = process.masterContext();
            masterCtx.playbackPositionSamples(renderTransport.playheadPosition.samples);
            // Offline render transport is private processing state. Plugins
            // must see a running transport to render correctly, but the shared
            // application timeline must remain stopped.
            masterCtx.isPlaying(
                timeline_.isPlaying || offlineRenderPlaying);
            uint32_t tempoMicros = static_cast<uint32_t>(60000000.0 / renderTransport.tempo);
            masterCtx.tempo(tempoMicros);
            masterCtx.timeSignatureNumerator(renderTransport.timeSignatureNumerator);
            masterCtx.timeSignatureDenominator(renderTransport.timeSignatureDenominator);

            // Process each timeline track into the target sequencer context.
            // targetSequence.tracks[i] points to a pump ring-buffer slot when called
            // from pumpAudio(), or to engine_.data().tracks[i] on the legacy path.
            if (!snapshot) return;
            for (size_t i = 0; i < snapshot->size() && i < targetSequence.tracks.size(); ++i) {
                auto* trackContext = targetSequence.tracks[i];
                if (!trackContext)
                    continue;

                // Clamp against the track's buffer capacity to prevent overruns
                const auto safeFrames = static_cast<int32_t>(std::min(
                    static_cast<size_t>(process.frameCount()),
                    trackContext->audioBufferCapacityInFrames()));

                // Copy device input channels
                if (process.audioInBusCount() > 0 && trackContext->audioInBusCount() > 0) {
                    const uint32_t deviceChannels = std::min(
                        static_cast<uint32_t>(process.inputChannelCount(0)),
                        static_cast<uint32_t>(trackContext->inputChannelCount(0))
                    );
                    for (uint32_t ch = 0; ch < deviceChannels; ++ch) {
                        const float* src = process.getFloatInBuffer(0, ch);
                        float* dst = trackContext->getFloatInBuffer(0, ch);
                        if (src && dst)
                            std::memcpy(dst, src, safeFrames * sizeof(float));
                    }
                }

                auto renderTimeline = renderTransport;
                TimelinePosition renderPosition{};
                const auto trackOffset = trackRenderOffsetInSamples(static_cast<int32_t>(i));
                const auto renderBaseSample =
                    (timeline_.isPlaying || offlineRenderPlaying ||
                     renderPlayheadRaw != audiblePlayheadSamples) ?
                        renderPlayheadRaw :
                        audiblePlayheadSamples;
                int64_t renderStartSample =
                    renderBaseSample + static_cast<int64_t>(trackOffset);
                if (renderStartSample < 0)
                    renderStartSample = 0;
                renderStartSample = wrapToLoopRange(renderStartSample);
                renderPosition.samples = renderStartSample;
                renderTimeline.seekTo(renderPosition, sampleRate_);
                updateTransportMetaForPlayhead(renderTimeline);

                int32_t destinationOffsetFrames = 0;
                int32_t remainingFrames = safeFrames;
                int64_t segmentStartSample = renderStartSample;
                while (remainingFrames > 0) {
                    auto segmentTimeline = renderTransport;
                    TimelinePosition segmentPosition{};
                    segmentPosition.samples = wrapToLoopRange(segmentStartSample);
                    segmentTimeline.seekTo(segmentPosition, sampleRate_);
                    updateTransportMetaForPlayhead(segmentTimeline);

                    int32_t segmentFrames = remainingFrames;
                    bool wrapsAtLoopEnd = false;
                    if (timeline_.loopEnabled &&
                        timeline_.loopEnd.samples > timeline_.loopStart.samples &&
                        segmentStartSample < timeline_.loopEnd.samples &&
                        segmentStartSample + remainingFrames > timeline_.loopEnd.samples) {
                        segmentFrames = static_cast<int32_t>(timeline_.loopEnd.samples - segmentStartSample);
                        wrapsAtLoopEnd = true;
                    }

                    if (segmentFrames <= 0)
                        break;

                    (*snapshot)[i]->processAudioForRenderSegment(
                        *trackContext,
                        segmentTimeline,
                        segmentStartSample,
                        destinationOffsetFrames,
                        segmentFrames);

                    destinationOffsetFrames += segmentFrames;
                    remainingFrames -= segmentFrames;
                    segmentStartSample = wrapsAtLoopEnd
                        ? timeline_.loopStart.samples
                        : (segmentStartSample + segmentFrames);
                }
            }
        }

        void onTrackAdded(
            uint32_t outputChannels,
            double sampleRate,
            uint32_t bufferSizeInFrames,
            int32_t insertionIndex) override {
            sampleRate_ = static_cast<int32_t>(sampleRate);
            bufferSizeInFrames_ = bufferSizeInFrames;
            master_timeline_track_->reconfigureBuffers(0, bufferSizeInFrames);

            std::string trackReferenceId;
            if (!pending_track_reference_id_.empty()) {
                trackReferenceId = std::move(pending_track_reference_id_);
                pending_track_reference_id_.clear();
                reserveTrackReferenceId(trackReferenceId);
            } else
                trackReferenceId = std::format("track_{}", next_timeline_track_reference_++);
            auto newTrack = std::make_shared<TimelineTrack>(trackReferenceId, outputChannels, sampleRate, bufferSizeInFrames);

            newTrack->setNrpnParameterCallback(
                [this, trackReferenceId](uint8_t group, uint32_t paramIdx, uint32_t rawValue, bool isRelative) {
                    auto& seqTracks = engine_.tracks();
                    uapmd_track_index_t trackIndex = -1;
                    auto tracks = this->tracks();
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        if (tracks[i] && tracks[i]->referenceId() == trackReferenceId) {
                            trackIndex = static_cast<uapmd_track_index_t>(i);
                            break;
                        }
                    }
                    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= seqTracks.size())
                        return;
                    auto* seqTrack = seqTracks[static_cast<size_t>(trackIndex)];
                    if (!seqTrack)
                        return;
                    for (int32_t instanceId : seqTrack->orderedInstanceIds()) {
                        // Only target the instance whose UMP group matches the event.
                        if (seqTrack->getInstanceGroup(instanceId) != group)
                            continue;
                        double value;
                        if (isRelative) {
                            auto* inst = engine_.getPluginInstance(instanceId);
                            if (!inst)
                                continue;
                            value = inst->getParameterValue(static_cast<int32_t>(paramIdx))
                                    + static_cast<double>(static_cast<int32_t>(rawValue)) / INT32_MAX;
                        } else {
                            value = static_cast<double>(rawValue) / UINT32_MAX;
                        }
                        plugin_parameter_mutation_depth_.fetch_add(1, std::memory_order_acq_rel);
                        engine_.setParameterValue(instanceId, static_cast<int32_t>(paramIdx), value);
                        plugin_parameter_mutation_depth_.fetch_sub(1, std::memory_order_acq_rel);
                    }
                });

            const auto trackIndex = insertionIndex < 0
                ? static_cast<int32_t>(timeline_tracks_.size())
                : insertionIndex;
            if (trackIndex < 0 || static_cast<size_t>(trackIndex) > timeline_tracks_.size())
                return;
            const auto eventTrackId = newTrack->referenceId();
            timeline_tracks_.insert(
                timeline_tracks_.begin() + trackIndex,
                std::move(newTrack));
            rebuildTrackSnapshot();

            ProjectDocumentEvent event(ProjectDocumentEventKind::TrackAdded, "track-added");
            event.setTrackId(eventTrackId)
                .setTrackIndex(trackIndex);
            emitProjectDocumentEvent(std::move(event));
        }

        void onTrackRemoved(size_t trackIndex) override {
            if (trackIndex < timeline_tracks_.size()) {
                const auto eventTrackId = timeline_tracks_[trackIndex]->referenceId();
                timeline_tracks_.erase(timeline_tracks_.begin() + static_cast<long>(trackIndex));
                rebuildTrackSnapshot();

                ProjectDocumentEvent event(ProjectDocumentEventKind::TrackRemoved, "track-removed");
                event.setTrackId(eventTrackId)
                    .setTrackIndex(static_cast<int32_t>(trackIndex));
                emitProjectDocumentEvent(std::move(event));
            }
        }

        void onTrackGraphChanged(int32_t trackIndex) override {
            ProjectDocumentEvent event(ProjectDocumentEventKind::PluginGraphChanged, "plugin-graph-changed");
            event.setTrackIndex(trackIndex);
            if (trackIndex == kMasterTrackIndex) {
                if (master_timeline_track_)
                    event.setTrackId(master_timeline_track_->referenceId());
            } else if (trackIndex >= 0 && trackIndex < static_cast<int32_t>(timeline_tracks_.size())) {
                if (auto& track = timeline_tracks_[static_cast<size_t>(trackIndex)])
                    event.setTrackId(track->referenceId());
            }
            emitProjectDocumentEvent(std::move(event));
        }

    private:
        static std::filesystem::path makeRelativePath(
            const std::filesystem::path& baseDir,
            const std::filesystem::path& target)
        {
            if (baseDir.empty() || target.empty())
                return target;

            std::error_code ec;
            auto rel = std::filesystem::relative(target, baseDir, ec);
            if (ec)
                return target;

            for (const auto& part : rel) {
                if (part == "..")
                    return target;
            }
            return rel;
        }

        static std::filesystem::path makeAbsolutePath(
            const std::filesystem::path& baseDir,
            const std::filesystem::path& target)
        {
            if (target.empty())
                return target;
            if (target.is_absolute() || baseDir.empty())
                return std::filesystem::absolute(target);
            return std::filesystem::absolute(baseDir / target);
        }

        static std::string urlEscapeFilenameComponent(std::string_view value) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string escaped;
            escaped.reserve(value.size() * 3);
            for (unsigned char ch : value) {
                if ((ch >= 'A' && ch <= 'Z') ||
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                    escaped.push_back(static_cast<char>(ch));
                    continue;
                }
                escaped.push_back('%');
                escaped.push_back(kHex[(ch >> 4) & 0xF]);
                escaped.push_back(kHex[ch & 0xF]);
            }
            return escaped;
        }

        static bool writeBinaryFile(
            const std::filesystem::path& path,
            const std::vector<uint8_t>& bytes,
            std::string& error)
        {
            std::error_code createDirEc;
            std::filesystem::create_directories(path.parent_path(), createDirEc);
            if (createDirEc) {
                error = std::format(
                    "Failed to create directory for {}: {}",
                    path.string(),
                    createDirEc.message());
                return false;
            }

            std::ofstream out(path, std::ios::binary);
            if (!out) {
                error = std::format("Failed to open {} for writing", path.string());
                return false;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                error = std::format("Failed to write {}", path.string());
                return false;
            }
            return true;
        }
    };

    std::unique_ptr<TimelineFacade> TimelineFacade::create(SequencerEngine& engine) {
        return std::make_unique<TimelineFacadeImpl>(engine);
    }

} // namespace uapmd
