#include "uapmd-engine/uapmd-engine.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace uapmd {

    namespace {
        ProjectUndoResult resultWithStatus(ProjectUndoStatus status, std::string error = {}) {
            return {
                .status = status,
                .error = std::move(error)
            };
        }
    }

    class ProjectUndoEngine::Impl : public std::enable_shared_from_this<ProjectUndoEngine::Impl> {
        enum class PendingDirection {
            Perform,
            Undo,
            Redo
        };

        struct Entry {
            std::shared_ptr<ProjectUndoableOperation> operation{};
            uint64_t beforeStateId{0};
            uint64_t afterStateId{0};
            size_t sizeInBytes{0};
        };

        struct Pending {
            uint64_t token{0};
            PendingDirection direction{PendingDirection::Perform};
            std::shared_ptr<ProjectUndoableOperation> operation{};
            ProjectUndoCompletion clientCompletion{};
        };

    public:
        explicit Impl(Configuration configuration)
            : maximum_history_size_in_bytes_(configuration.maximumHistorySizeInBytes)
            , dispatch_to_model_thread_(std::move(configuration.dispatchToModelThread)) {
            if (!dispatch_to_model_thread_)
                dispatch_to_model_thread_ = [](ProjectUndoTask task) {
                    if (task)
                        task();
                };
        }

        ProjectUndoState state() const {
            return {
                .busy = pending_.has_value(),
                .canUndo = !pending_ && !undo_stack_.empty() && !stopped_,
                .canRedo = !pending_ && !redo_stack_.empty() && !stopped_,
                .dirty = current_state_id_ != saved_state_id_,
                .undoDescription = undo_stack_.empty()
                    ? std::string{}
                    : undo_stack_.back().operation->description(),
                .redoDescription = redo_stack_.empty()
                    ? std::string{}
                    : redo_stack_.back().operation->description(),
                .historySizeInBytes = history_size_in_bytes_,
                .maximumHistorySizeInBytes = maximum_history_size_in_bytes_,
                .currentStateId = current_state_id_,
                .savedStateId = saved_state_id_
            };
        }

        void perform(
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectUndoCompletion completion) {
            if (!operation) {
                completeClient(
                    std::move(completion),
                    ProjectUndoResult::failure("Cannot perform an empty undo operation."));
                return;
            }
            if (!begin(PendingDirection::Perform, operation, std::move(completion)))
                return;

            auto context = executionContext(ProjectMutationOrigin::User, true);
            operation->perform(context, operationCompletion(pending_->token));
        }

        void undo(ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return;
            }
            if (pending_) {
                completeClient(std::move(completion), resultWithStatus(
                    ProjectUndoStatus::Busy,
                    "An undo history operation is already pending."));
                return;
            }
            if (undo_stack_.empty()) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::NothingToUndo));
                return;
            }

            auto operation = undo_stack_.back().operation;
            begin(PendingDirection::Undo, operation, std::move(completion));
            auto context = executionContext(ProjectMutationOrigin::UndoRedo, false);
            operation->undo(context, operationCompletion(pending_->token));
        }

        void redo(ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return;
            }
            if (pending_) {
                completeClient(std::move(completion), resultWithStatus(
                    ProjectUndoStatus::Busy,
                    "An undo history operation is already pending."));
                return;
            }
            if (redo_stack_.empty()) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::NothingToRedo));
                return;
            }

            auto operation = redo_stack_.back().operation;
            begin(PendingDirection::Redo, operation, std::move(completion));
            auto context = executionContext(ProjectMutationOrigin::UndoRedo, false);
            operation->redo(context, operationCompletion(pending_->token));
        }

        bool clear(bool markCurrentStateSaved) {
            if (pending_ || stopped_)
                return false;
            clearEntries(undo_stack_);
            clearEntries(redo_stack_);
            current_state_id_ = next_state_id_++;
            if (markCurrentStateSaved)
                saved_state_id_ = current_state_id_;
            return true;
        }

        bool markSaved() {
            if (pending_ || stopped_)
                return false;
            saved_state_id_ = current_state_id_;
            return true;
        }

        bool setMaximumHistorySizeInBytes(size_t value) {
            if (pending_ || stopped_)
                return false;
            maximum_history_size_in_bytes_ = value;
            enforceMemoryBudget();
            return true;
        }

        void shutdown() {
            if (stopped_)
                return;
            stopped_ = true;
            ++operation_token_;

            ProjectUndoCompletion completion;
            if (pending_) {
                completion = std::move(pending_->clientCompletion);
                pending_.reset();
            }
            clearEntries(undo_stack_);
            clearEntries(redo_stack_);
            completeClient(std::move(completion), resultWithStatus(
                ProjectUndoStatus::Cancelled,
                "The undo engine was shut down while an operation was pending."));
        }

    private:
        bool begin(
            PendingDirection direction,
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return false;
            }
            if (pending_) {
                completeClient(std::move(completion), resultWithStatus(
                    ProjectUndoStatus::Busy,
                    "An undo history operation is already pending."));
                return false;
            }

            pending_ = Pending{
                .token = ++operation_token_,
                .direction = direction,
                .operation = std::move(operation),
                .clientCompletion = std::move(completion)
            };
            return true;
        }

        ProjectUndoExecutionContext executionContext(
            ProjectMutationOrigin origin,
            bool recordHistory) const {
            return {
                .origin = origin,
                .recordHistory = recordHistory,
                .dispatchToModelThread = dispatch_to_model_thread_
            };
        }

        ProjectUndoCompletion operationCompletion(uint64_t token) {
            std::weak_ptr<Impl> weakSelf = shared_from_this();
            return [weakSelf, token](ProjectUndoResult result) mutable {
                auto self = weakSelf.lock();
                if (!self)
                    return;
                self->dispatch_to_model_thread_(
                    [weakSelf, token, result = std::move(result)]() mutable {
                        if (auto locked = weakSelf.lock())
                            locked->finish(token, std::move(result));
                    });
            };
        }

        void finish(uint64_t token, ProjectUndoResult result) {
            if (!pending_ || pending_->token != token || stopped_)
                return;

            auto pending = std::move(*pending_);
            pending_.reset();

            if (result.succeeded()) {
                switch (pending.direction) {
                    case PendingDirection::Perform:
                        finishPerform(std::move(pending.operation));
                        break;
                    case PendingDirection::Undo:
                        finishUndo();
                        break;
                    case PendingDirection::Redo:
                        finishRedo();
                        break;
                }
            }

            completeClient(std::move(pending.clientCompletion), std::move(result));
        }

        void finishPerform(std::shared_ptr<ProjectUndoableOperation> operation) {
            clearEntries(redo_stack_);
            const auto size = operation->historySizeInBytes();
            const auto beforeStateId = current_state_id_;
            const auto afterStateId = next_state_id_++;
            undo_stack_.push_back({
                .operation = std::move(operation),
                .beforeStateId = beforeStateId,
                .afterStateId = afterStateId,
                .sizeInBytes = size
            });
            history_size_in_bytes_ += size;
            current_state_id_ = afterStateId;
            enforceMemoryBudget();
        }

        void finishUndo() {
            auto entry = std::move(undo_stack_.back());
            undo_stack_.pop_back();
            current_state_id_ = entry.beforeStateId;
            redo_stack_.push_back(std::move(entry));
        }

        void finishRedo() {
            auto entry = std::move(redo_stack_.back());
            redo_stack_.pop_back();
            current_state_id_ = entry.afterStateId;
            undo_stack_.push_back(std::move(entry));
        }

        void enforceMemoryBudget() {
            // Retain the newest action even when it alone exceeds the budget;
            // otherwise a successful edit could become non-undoable immediately.
            while (history_size_in_bytes_ > maximum_history_size_in_bytes_
                && undo_stack_.size() > 1) {
                history_size_in_bytes_ -= undo_stack_.front().sizeInBytes;
                undo_stack_.erase(undo_stack_.begin());
            }
        }

        void clearEntries(std::vector<Entry>& entries) {
            for (const auto& entry : entries)
                history_size_in_bytes_ -= std::min(history_size_in_bytes_, entry.sizeInBytes);
            entries.clear();
        }

        static void completeClient(
            ProjectUndoCompletion completion,
            ProjectUndoResult result) {
            if (completion)
                completion(std::move(result));
        }

        size_t maximum_history_size_in_bytes_{0};
        size_t history_size_in_bytes_{0};
        ProjectModelThreadDispatcher dispatch_to_model_thread_{};
        std::vector<Entry> undo_stack_{};
        std::vector<Entry> redo_stack_{};
        std::optional<Pending> pending_{};
        uint64_t operation_token_{0};
        uint64_t next_state_id_{2};
        uint64_t current_state_id_{1};
        uint64_t saved_state_id_{1};
        bool stopped_{false};
    };

    ProjectUndoEngine::ProjectUndoEngine()
        : ProjectUndoEngine(Configuration{}) {
    }

    ProjectUndoEngine::ProjectUndoEngine(Configuration configuration)
        : impl_(std::make_shared<Impl>(std::move(configuration))) {
    }

    ProjectUndoEngine::~ProjectUndoEngine() {
        shutdown();
    }

    ProjectUndoState ProjectUndoEngine::state() const {
        return impl_->state();
    }

    void ProjectUndoEngine::perform(
        std::shared_ptr<ProjectUndoableOperation> operation,
        ProjectUndoCompletion completion) {
        impl_->perform(std::move(operation), std::move(completion));
    }

    void ProjectUndoEngine::undo(ProjectUndoCompletion completion) {
        impl_->undo(std::move(completion));
    }

    void ProjectUndoEngine::redo(ProjectUndoCompletion completion) {
        impl_->redo(std::move(completion));
    }

    bool ProjectUndoEngine::clear(bool markCurrentStateSaved) {
        return impl_->clear(markCurrentStateSaved);
    }

    bool ProjectUndoEngine::markSaved() {
        return impl_->markSaved();
    }

    bool ProjectUndoEngine::setMaximumHistorySizeInBytes(size_t value) {
        return impl_->setMaximumHistorySizeInBytes(value);
    }

    void ProjectUndoEngine::shutdown() {
        if (impl_)
            impl_->shutdown();
    }

} // namespace uapmd
