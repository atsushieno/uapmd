#include "uapmd-engine/uapmd-engine.hpp"

#include <algorithm>
#include <optional>
#include <thread>
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

        class CompoundUndoOperation final
            : public ProjectUndoableOperation,
              public std::enable_shared_from_this<CompoundUndoOperation> {
            enum class Direction {
                Perform,
                Undo,
                Redo
            };

            struct RunState {
                Direction direction{Direction::Perform};
                ProjectUndoExecutionContext context{};
                ProjectUndoCompletion completion{};
                std::vector<size_t> order{};
                std::vector<size_t> completed{};
                size_t cursor{0};
                ProjectUndoResult failure{};
            };

        public:
            CompoundUndoOperation(
                std::string description,
                std::vector<std::shared_ptr<ProjectUndoableOperation>> children)
                : description_(std::move(description))
                , children_(std::move(children)) {
            }

            std::string description() const override {
                return description_;
            }

            size_t historySizeInBytes() const override {
                size_t result = sizeof(*this)
                    + description_.capacity()
                    + children_.capacity() * sizeof(children_.front());
                for (const auto& child : children_)
                    if (child)
                        result += child->historySizeInBytes();
                return result;
            }

            void perform(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(Direction::Perform, context, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(Direction::Undo, context, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(Direction::Redo, context, std::move(completion));
            }

        private:
            void run(
                Direction direction,
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) {
                auto state = std::make_shared<RunState>();
                state->direction = direction;
                state->context = context;
                state->context.recordHistory = false;
                state->completion = std::move(completion);
                state->order.reserve(children_.size());
                if (direction == Direction::Undo) {
                    for (size_t index = children_.size(); index > 0; --index)
                        state->order.push_back(index - 1);
                } else {
                    for (size_t index = 0; index < children_.size(); ++index)
                        state->order.push_back(index);
                }
                runNext(std::move(state));
            }

            void runNext(std::shared_ptr<RunState> state) {
                if (state->cursor >= state->order.size()) {
                    complete(std::move(state), ProjectUndoResult::success());
                    return;
                }

                const auto childIndex = state->order[state->cursor];
                auto child = children_[childIndex];
                if (!child) {
                    beginCompensation(
                        std::move(state),
                        ProjectUndoResult::failure("A compound undo step contains an empty operation."));
                    return;
                }

                auto self = shared_from_this();
                auto childCompletion = [self, state, childIndex](ProjectUndoResult result) mutable {
                    state->context.dispatch(
                        [self, state, childIndex, result = std::move(result)]() mutable {
                            if (!result.succeeded()) {
                                self->beginCompensation(std::move(state), std::move(result));
                                return;
                            }
                            state->completed.push_back(childIndex);
                            ++state->cursor;
                            self->runNext(std::move(state));
                        });
                };
                invoke(*child, state->direction, state->context, std::move(childCompletion));
            }

            void beginCompensation(
                std::shared_ptr<RunState> state,
                ProjectUndoResult failure) {
                state->failure = std::move(failure);
                compensateNext(std::move(state));
            }

            void compensateNext(std::shared_ptr<RunState> state) {
                if (state->completed.empty()) {
                    auto failure = std::move(state->failure);
                    complete(std::move(state), std::move(failure));
                    return;
                }

                const auto childIndex = state->completed.back();
                state->completed.pop_back();
                auto child = children_[childIndex];
                const auto compensationDirection = state->direction == Direction::Undo
                    ? Direction::Redo
                    : Direction::Undo;
                auto self = shared_from_this();
                auto childCompletion = [self, state, childIndex](ProjectUndoResult result) mutable {
                    state->context.dispatch(
                        [self, state, childIndex, result = std::move(result)]() mutable {
                            if (!result.succeeded()) {
                                if (!state->failure.error.empty())
                                    state->failure.error += " ";
                                state->failure.error += "Compensation failed for child "
                                    + std::to_string(childIndex) + ": " + result.error;
                                auto failure = std::move(state->failure);
                                self->complete(std::move(state), std::move(failure));
                                return;
                            }
                            self->compensateNext(std::move(state));
                        });
                };
                invoke(*child, compensationDirection, state->context, std::move(childCompletion));
            }

            static void invoke(
                ProjectUndoableOperation& operation,
                Direction direction,
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) {
                switch (direction) {
                    case Direction::Perform:
                        operation.perform(context, std::move(completion));
                        break;
                    case Direction::Undo:
                        operation.undo(context, std::move(completion));
                        break;
                    case Direction::Redo:
                        operation.redo(context, std::move(completion));
                        break;
                }
            }

            static void complete(
                std::shared_ptr<RunState> state,
                ProjectUndoResult result) {
                auto completion = std::move(state->completion);
                if (completion)
                    completion(std::move(result));
            }

            std::string description_{};
            std::vector<std::shared_ptr<ProjectUndoableOperation>> children_{};
        };
    }

    class ProjectUndoEngine::Impl : public std::enable_shared_from_this<ProjectUndoEngine::Impl> {
        enum class PendingDirection {
            Perform,
            Undo,
            Redo,
            CancelCompound
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

        struct CompoundBuilder {
            std::string description{};
            ProjectMutationOrigin origin{ProjectMutationOrigin::User};
            std::vector<std::shared_ptr<ProjectUndoableOperation>> children{};
        };

    public:
        explicit Impl(Configuration configuration)
            : maximum_history_size_in_bytes_(configuration.maximumHistorySizeInBytes)
            , dispatch_to_model_thread_(std::move(configuration.dispatchToModelThread))
            , model_thread_id_(std::this_thread::get_id()) {
            if (!dispatch_to_model_thread_)
                dispatch_to_model_thread_ = [](ProjectUndoTask task) {
                    if (task)
                        task();
                };
        }

        ProjectUndoState state() const {
            size_t retainedHistorySize = history_size_in_bytes_;
            if (compound_)
                for (const auto& child : compound_->children)
                    if (child)
                        retainedHistorySize += child->historySizeInBytes();
            return {
                .busy = pending_.has_value() || compound_.has_value(),
                .compoundOpen = compound_.has_value(),
                .canUndo = !pending_ && !compound_ && !undo_stack_.empty() && !stopped_,
                .canRedo = !pending_ && !compound_ && !redo_stack_.empty() && !stopped_,
                .dirty = compound_.has_value() || current_state_id_ != saved_state_id_,
                .compoundDescription = compound_ ? compound_->description : std::string{},
                .undoDescription = undo_stack_.empty()
                    ? std::string{}
                    : undo_stack_.back().operation->description(),
                .redoDescription = redo_stack_.empty()
                    ? std::string{}
                    : redo_stack_.back().operation->description(),
                .historySizeInBytes = retainedHistorySize,
                .maximumHistorySizeInBytes = maximum_history_size_in_bytes_,
                .currentStateId = current_state_id_,
                .savedStateId = saved_state_id_
            };
        }

        void perform(
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion) {
            if (!operation) {
                completeClient(
                    std::move(completion),
                    ProjectUndoResult::failure("Cannot perform an empty undo operation."));
                return;
            }
            if (compound_ && compound_->origin != origin) {
                completeClient(
                    std::move(completion),
                    ProjectUndoResult::failure(
                        "An operation origin cannot change inside a compound undo step."));
                return;
            }
            if (!begin(PendingDirection::Perform, operation, std::move(completion)))
                return;

            auto context = executionContext(origin, true);
            operation->perform(context, operationCompletion(pending_->token));
        }

        void undo(ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return;
            }
            if (pending_ || compound_) {
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
            if (pending_ || compound_) {
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

        ProjectUndoResult beginCompound(
            std::string description,
            ProjectMutationOrigin origin) {
            if (stopped_)
                return resultWithStatus(ProjectUndoStatus::Stopped);
            if (pending_ || compound_)
                return resultWithStatus(
                    ProjectUndoStatus::Busy,
                    compound_
                        ? "A compound undo step is already open."
                        : "An undo history operation is already pending.");
            if (description.empty())
                return ProjectUndoResult::failure("A compound undo step requires a description.");
            compound_ = CompoundBuilder{
                .description = std::move(description),
                .origin = origin
            };
            return ProjectUndoResult::success();
        }

        void endCompound(ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return;
            }
            if (pending_) {
                completeClient(std::move(completion), resultWithStatus(
                    ProjectUndoStatus::Busy,
                    "An operation in the compound undo step is still pending."));
                return;
            }
            if (!compound_) {
                completeClient(
                    std::move(completion),
                    ProjectUndoResult::failure("No compound undo step is open."));
                return;
            }

            auto compound = std::move(*compound_);
            compound_.reset();
            if (compound.children.empty()) {
                completeClient(std::move(completion), ProjectUndoResult::success());
                return;
            }
            auto operation = std::make_shared<CompoundUndoOperation>(
                std::move(compound.description),
                std::move(compound.children));
            finishPerform(std::move(operation));
            completeClient(std::move(completion), ProjectUndoResult::success());
        }

        void cancelCompound(ProjectUndoCompletion completion) {
            if (stopped_) {
                completeClient(std::move(completion), resultWithStatus(ProjectUndoStatus::Stopped));
                return;
            }
            if (pending_) {
                completeClient(std::move(completion), resultWithStatus(
                    ProjectUndoStatus::Busy,
                    "An operation in the compound undo step is still pending."));
                return;
            }
            if (!compound_) {
                completeClient(
                    std::move(completion),
                    ProjectUndoResult::failure("No compound undo step is open."));
                return;
            }
            if (compound_->children.empty()) {
                compound_.reset();
                completeClient(std::move(completion), ProjectUndoResult::success());
                return;
            }

            auto operation = std::make_shared<CompoundUndoOperation>(
                compound_->description,
                compound_->children);
            if (!begin(PendingDirection::CancelCompound, operation, std::move(completion)))
                return;
            auto context = executionContext(ProjectMutationOrigin::UndoRedo, false);
            operation->undo(context, operationCompletion(pending_->token));
        }

        bool clear(bool markCurrentStateSaved) {
            if (pending_ || compound_ || stopped_)
                return false;
            clearEntries(undo_stack_);
            clearEntries(redo_stack_);
            current_state_id_ = next_state_id_++;
            if (markCurrentStateSaved)
                saved_state_id_ = current_state_id_;
            return true;
        }

        bool markSaved() {
            if (compound_)
                return false;
            return markStateSaved(current_state_id_);
        }

        bool markStateSaved(uint64_t stateId) {
            if (stopped_)
                return false;
            saved_state_id_ = stateId;
            return true;
        }

        bool setMaximumHistorySizeInBytes(size_t value) {
            if (pending_ || compound_ || stopped_)
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
            compound_.reset();
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
                if (std::this_thread::get_id() == self->model_thread_id_) {
                    self->finish(token, std::move(result));
                    return;
                }
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
                        if (compound_)
                            compound_->children.push_back(std::move(pending.operation));
                        else
                            finishPerform(std::move(pending.operation));
                        break;
                    case PendingDirection::Undo:
                        finishUndo();
                        break;
                    case PendingDirection::Redo:
                        finishRedo();
                        break;
                    case PendingDirection::CancelCompound:
                        compound_.reset();
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
        std::thread::id model_thread_id_{};
        std::vector<Entry> undo_stack_{};
        std::vector<Entry> redo_stack_{};
        std::optional<Pending> pending_{};
        std::optional<CompoundBuilder> compound_{};
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
        impl_->perform(
            std::move(operation),
            ProjectMutationOrigin::User,
            std::move(completion));
    }

    void ProjectUndoEngine::perform(
        std::shared_ptr<ProjectUndoableOperation> operation,
        ProjectMutationOrigin origin,
        ProjectUndoCompletion completion) {
        impl_->perform(std::move(operation), origin, std::move(completion));
    }

    void ProjectUndoEngine::undo(ProjectUndoCompletion completion) {
        impl_->undo(std::move(completion));
    }

    void ProjectUndoEngine::redo(ProjectUndoCompletion completion) {
        impl_->redo(std::move(completion));
    }

    ProjectUndoResult ProjectUndoEngine::beginCompound(
        std::string description,
        ProjectMutationOrigin origin) {
        return impl_->beginCompound(std::move(description), origin);
    }

    void ProjectUndoEngine::endCompound(ProjectUndoCompletion completion) {
        impl_->endCompound(std::move(completion));
    }

    void ProjectUndoEngine::cancelCompound(ProjectUndoCompletion completion) {
        impl_->cancelCompound(std::move(completion));
    }

    bool ProjectUndoEngine::clear(bool markCurrentStateSaved) {
        return impl_->clear(markCurrentStateSaved);
    }

    bool ProjectUndoEngine::markSaved() {
        return impl_->markSaved();
    }

    bool ProjectUndoEngine::markStateSaved(uint64_t stateId) {
        return impl_->markStateSaved(stateId);
    }

    bool ProjectUndoEngine::setMaximumHistorySizeInBytes(size_t value) {
        return impl_->setMaximumHistorySizeInBytes(value);
    }

    void ProjectUndoEngine::shutdown() {
        if (impl_)
            impl_->shutdown();
    }

} // namespace uapmd
