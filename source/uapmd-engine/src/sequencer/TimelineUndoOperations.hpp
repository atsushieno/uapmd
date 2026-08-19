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
            bool bypassed,
            uint8_t group,
            const std::vector<uint8_t>& state,
            const std::vector<uapmd_graph::AudioPluginGraphConnection>& connections,
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
            bool bypassed,
            uint8_t group,
            std::vector<uint8_t> state,
            std::vector<uapmd_graph::AudioPluginGraphConnection> connections,
            Add add,
            Remove remove)
            : initially_added_(initiallyAdded)
            , track_reference_id_(std::move(trackReferenceId))
            , format_(std::move(format))
            , plugin_id_(std::move(pluginId))
            , node_id_(std::move(nodeId))
            , bypassed_(bypassed)
            , group_(group)
            , state_(std::move(state))
            , connections_(std::move(connections))
            , add_(std::move(add))
            , remove_(std::move(remove)) {
        }

        std::string description() const override {
            return initially_added_ ? "Add plug-in" : "Remove plug-in";
        }

        size_t historySizeInBytes() const override {
            auto result = sizeof(*this)
                + track_reference_id_.capacity()
                + format_.capacity()
                + plugin_id_.capacity()
                + node_id_.capacity()
                + state_.capacity()
                + connections_.capacity()
                    * sizeof(uapmd_graph::AudioPluginGraphConnection);
            for (const auto& connection : connections_)
                result += connection.source.node_id.capacity()
                    + connection.target.node_id.capacity();
            return result;
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
            add_(track_reference_id_, format_, plugin_id_, node_id_, bypassed_, group_, state_, connections_,
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
        bool bypassed_{false};
        uint8_t group_{0xFF};
        std::vector<uint8_t> state_{};
        std::vector<uapmd_graph::AudioPluginGraphConnection> connections_{};
        Add add_{};
        Remove remove_{};
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
} // namespace uapmd::timeline_detail
