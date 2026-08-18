#pragma once

// Structural undo operations that have not been converted to commands, and
// the plug-in mutation scope they share with the facade.
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
        using Redo = std::function<void(ProjectUndoCompletion completion)>;

        PluginStateUndoOperation(
            std::string trackReferenceId,
            std::string nodeId,
            std::vector<uint8_t> before,
            std::vector<uint8_t> after,
            Apply apply,
            std::string description = "Load plug-in state",
            Redo redo = {})
            : track_reference_id_(std::move(trackReferenceId))
            , node_id_(std::move(nodeId))
            , before_(std::move(before))
            , after_(std::move(after))
            , apply_(std::move(apply))
            , description_(std::move(description))
            , redo_(std::move(redo)) {
        }

        std::string description() const override {
            return description_;
        }

        size_t historySizeInBytes() const override {
            return sizeof(*this)
                + track_reference_id_.capacity()
                + node_id_.capacity()
                + before_.capacity()
                + after_.capacity()
                + description_.capacity();
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
            if (redo_) {
                redo_(std::move(completion));
                return;
            }
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
        std::string description_{};
        Redo redo_{};
    };

    class PluginInstanceUndoOperation final : public ProjectUndoableOperation {
    public:
        using Add = std::function<void(
            std::string_view trackReferenceId,
            std::string_view format,
            std::string_view pluginId,
            std::string_view nodeId,
            uint8_t group,
            const std::vector<uint8_t>& state,
            std::function<void(int32_t, std::string)> completion)>;
        using Remove = std::function<void(
            std::string_view trackReferenceId,
            std::string_view nodeId,
            std::function<void(std::string)> completion)>;

        PluginInstanceUndoOperation(
            bool initiallyAdded,
            std::string trackReferenceId,
            std::string format,
            std::string pluginId,
            std::string nodeId,
            uint8_t group,
            std::vector<uint8_t> state,
            Add add,
            Remove remove)
            : initially_added_(initiallyAdded)
            , track_reference_id_(std::move(trackReferenceId))
            , format_(std::move(format))
            , plugin_id_(std::move(pluginId))
            , node_id_(std::move(nodeId))
            , group_(group)
            , state_(std::move(state))
            , add_(std::move(add))
            , remove_(std::move(remove)) {
        }

        std::string description() const override {
            return initially_added_ ? "Add plug-in" : "Remove plug-in";
        }

        size_t historySizeInBytes() const override {
            return sizeof(*this)
                + track_reference_id_.capacity()
                + format_.capacity()
                + plugin_id_.capacity()
                + node_id_.capacity()
                + state_.capacity();
        }

        void perform(
            const ProjectUndoExecutionContext&,
            ProjectUndoCompletion completion) override {
            if (initially_added_)
                remove(std::move(completion));
            else
                add(std::move(completion));
        }

        void undo(
            const ProjectUndoExecutionContext&,
            ProjectUndoCompletion completion) override {
            if (initially_added_)
                remove(std::move(completion));
            else
                add(std::move(completion));
        }

        void redo(
            const ProjectUndoExecutionContext&,
            ProjectUndoCompletion completion) override {
            if (initially_added_)
                add(std::move(completion));
            else
                remove(std::move(completion));
        }

    private:
        void remove(ProjectUndoCompletion completion) {
            if (!remove_) {
                if (completion)
                    completion(ProjectUndoResult::failure("Plug-in removal is unavailable"));
                return;
            }
            remove_(
                track_reference_id_,
                node_id_,
                [completion = std::move(completion)](std::string error) mutable {
                    if (!error.empty()) {
                        if (completion)
                            completion(ProjectUndoResult::failure(std::move(error)));
                        return;
                    }
                    if (completion)
                        completion(ProjectUndoResult::success());
                });
        }

        void add(ProjectUndoCompletion completion) {
            if (!add_) {
                if (completion)
                    completion(ProjectUndoResult::failure("Plug-in restoration is unavailable"));
                return;
            }
            add_(track_reference_id_, format_, plugin_id_, node_id_, group_, state_,
                 [completion = std::move(completion)](int32_t instanceId, std::string error) mutable {
                if (!error.empty() || instanceId < 0) {
                    if (completion)
                        completion(ProjectUndoResult::failure(
                            error.empty() ? "Could not restore plug-in" : std::move(error)));
                    return;
                }
                if (completion)
                    completion(ProjectUndoResult::success());
            });
        }

        bool initially_added_{false};
        std::string track_reference_id_{};
        std::string format_{};
        std::string plugin_id_{};
        std::string node_id_{};
        uint8_t group_{0xFF};
        std::vector<uint8_t> state_{};
        Add add_{};
        Remove remove_{};
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
    // Raises the plug-in mutation depths while an asynchronous plug-in
    // mutation is in flight, so that notifications the plug-in sends back
    // are recognised as our own rather than as external user edits. Held
    // by shared_ptr because the scope spans callbacks and outlives the
    // call that opened it.
    class PluginMutationScope {
        std::atomic<uint32_t>* parameter_depth_;
        std::atomic<uint32_t>* state_depth_;

    public:
        PluginMutationScope(
            std::atomic<uint32_t>* parameterDepth,
            std::atomic<uint32_t>& stateDepth)
            : parameter_depth_(parameterDepth)
            , state_depth_(&stateDepth) {
            if (parameter_depth_)
                parameter_depth_->fetch_add(1, std::memory_order_acq_rel);
            state_depth_->fetch_add(1, std::memory_order_acq_rel);
        }

        ~PluginMutationScope() {
            if (parameter_depth_)
                parameter_depth_->fetch_sub(1, std::memory_order_acq_rel);
            state_depth_->fetch_sub(1, std::memory_order_acq_rel);
        }

        PluginMutationScope(const PluginMutationScope&) = delete;
        PluginMutationScope& operator=(const PluginMutationScope&) = delete;
    };

    using PluginMutationScopePtr = std::shared_ptr<PluginMutationScope>;

} // namespace uapmd::timeline_detail
