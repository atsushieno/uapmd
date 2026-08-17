#include "uapmd-data/uapmd-data.hpp"

#include <optional>
#include <thread>
#include <utility>

namespace uapmd {

    namespace {
        // History records only what the user or a remote client asked for.
        // Loading a project, replaying history, and internal bookkeeping all
        // change the document without becoming undoable steps of their own.
        bool recordsHistory(ProjectMutationOrigin origin) {
            return origin == ProjectMutationOrigin::User
                || origin == ProjectMutationOrigin::Remote;
        }

        // Opens a document transaction for as long as it lives. Transactions
        // nest, so a command inside a step simply adds a level.
        class ScopedTransaction {
            std::function<void()> end_;

        public:
            ScopedTransaction(
                const std::function<void()>& begin,
                std::function<void()> end)
                : end_(begin && end ? std::move(end) : std::function<void()>{}) {
                if (end_)
                    begin();
            }

            ~ScopedTransaction() {
                if (end_)
                    end_();
            }

            ScopedTransaction(const ScopedTransaction&) = delete;
            ScopedTransaction& operator=(const ScopedTransaction&) = delete;
        };

        class CommandExecutionContext final : public ProjectCommandContext {
            ProjectMutationOrigin origin_;
            bool replaying_;
            ProjectModelThreadDispatcher dispatch_;
            ProjectCommandPtr revert_{};

        public:
            CommandExecutionContext(
                ProjectMutationOrigin origin,
                bool replaying,
                ProjectModelThreadDispatcher dispatch)
                : origin_(origin)
                , replaying_(replaying)
                , dispatch_(std::move(dispatch)) {
            }

            ProjectMutationOrigin origin() const override {
                return origin_;
            }

            bool replaying() const override {
                return replaying_;
            }

            void recordRevert(ProjectCommandPtr revert) override {
                revert_ = std::move(revert);
            }

            void dispatchToModelThread(ProjectUndoTask task) override {
                if (dispatch_)
                    dispatch_(std::move(task));
                else if (task)
                    task();
            }

            ProjectCommandPtr takeRevert() {
                return std::move(revert_);
            }
        };

        // Adapts a pair of mutually reversing commands to the history engine.
        //
        // Each run refreshes the command for the opposite direction from
        // whatever the executed command registered. Undo therefore needs no
        // special case: it runs the reverting command, which registers the
        // original as its own revert, which redo then runs.
        class CommandOperation final
            : public ProjectUndoableOperation
            , public std::enable_shared_from_this<CommandOperation> {

            ProjectCommandPtr forward_;
            ProjectCommandPtr backward_{};
            ProjectModelThreadDispatcher dispatch_;
            std::function<void()> begin_transaction_;
            std::function<void()> end_transaction_;

        public:
            CommandOperation(
                ProjectCommandPtr command,
                ProjectModelThreadDispatcher dispatch,
                std::function<void()> beginTransaction,
                std::function<void()> endTransaction)
                : forward_(std::move(command))
                , dispatch_(std::move(dispatch))
                , begin_transaction_(std::move(beginTransaction))
                , end_transaction_(std::move(endTransaction)) {
            }

            std::string description() const override {
                return forward_ ? forward_->description() : std::string{};
            }

            size_t historySizeInBytes() const override {
                size_t result = sizeof(*this);
                if (forward_)
                    result += forward_->retainedSizeInBytes();
                if (backward_)
                    result += backward_->retainedSizeInBytes();
                return result;
            }

            // A command that registered no revert changed nothing, so the
            // history engine drops the entry. This is what replaces the
            // "if (before == after) return true;" early-out that every
            // mutation entry point would otherwise have to write for itself.
            bool hasEffect() const override {
                return backward_ != nullptr;
            }

            bool mergeWith(const ProjectUndoableOperation& subsequent) override {
                const auto* next = dynamic_cast<const CommandOperation*>(&subsequent);
                if (!next || !next->forward_ || !forward_)
                    return false;
                if (next->forward_->commandId() != forward_->commandId())
                    return false;
                // Adopt the later payload while keeping the earliest revert,
                // so the coalesced step still restores the value the gesture
                // started from.
                return forward_->mergeWith(*next->forward_);
            }

            void perform(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(forward_, backward_, context, std::move(completion));
            }

            void undo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(backward_, forward_, context, std::move(completion));
            }

            void redo(
                const ProjectUndoExecutionContext& context,
                ProjectUndoCompletion completion) override {
                run(forward_, backward_, context, std::move(completion));
            }

        private:
            void run(
                const ProjectCommandPtr& command,
                ProjectCommandPtr& opposite,
                const ProjectUndoExecutionContext& engineContext,
                ProjectUndoCompletion completion) {
                if (!command) {
                    if (completion)
                        completion(ProjectUndoResult::failure(
                            "The command for this history step is missing."));
                    return;
                }

                // The context outlives this call: an asynchronous command
                // records its revert from a callback.
                auto context = std::make_shared<CommandExecutionContext>(
                    engineContext.origin,
                    !engineContext.recordHistory,
                    dispatch_);
                auto transaction = command->batchesDocumentEvents()
                    ? std::make_shared<ScopedTransaction>(
                        begin_transaction_, end_transaction_)
                    : nullptr;
                auto self = shared_from_this();
                auto* target = &opposite;
                command->execute(
                    *context,
                    [self, context, transaction, target,
                     completion = std::move(completion)](
                        ProjectCommandResult result) mutable {
                        if (result.succeeded())
                            if (auto revert = context->takeRevert())
                                *target = std::move(revert);
                        transaction.reset();
                        if (completion)
                            completion(std::move(result));
                    });
            }
        };
    }

    class ProjectCommandManager::Impl {
    public:
        explicit Impl(Configuration configuration)
            : history_(configuration.history)
            , dispatch_(std::move(configuration.dispatchToModelThread))
            , begin_transaction_(std::move(configuration.beginDocumentTransaction))
            , end_transaction_(std::move(configuration.endDocumentTransaction))
            , model_thread_id_(std::this_thread::get_id()) {
        }

        void execute(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) {
            if (!command) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "Cannot execute an empty command."));
                return;
            }
            if (!history_) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "The command manager has no history engine."));
                return;
            }

            // Applied without recording: project loading, history replay, and
            // internal bookkeeping change the document but are not user steps.
            if (!recordsHistory(origin)) {
                applyWithoutRecording(std::move(command), origin, std::move(completion));
                return;
            }

            history_->perform(
                std::make_shared<CommandOperation>(
                    std::move(command),
                    dispatch_,
                    begin_transaction_,
                    end_transaction_),
                origin,
                std::move(completion));
        }

        ProjectCommandResult executeSynchronously(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin) {
            if (std::this_thread::get_id() != model_thread_id_)
                return {
                    .status = ProjectUndoStatus::Busy,
                    .error = "A command can only be executed synchronously on the model thread."
                };

            std::optional<ProjectCommandResult> result;
            execute(
                std::move(command),
                origin,
                [&result](ProjectCommandResult completed) {
                    result = std::move(completed);
                });
            if (!result.has_value())
                return {
                    .status = ProjectUndoStatus::Busy,
                    .error = "The command did not complete synchronously."
                };
            return std::move(*result);
        }

        void undo(ProjectCommandCompletion completion) {
            if (!history_) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "The command manager has no history engine."));
                return;
            }
            history_->undo(std::move(completion));
        }

        void redo(ProjectCommandCompletion completion) {
            if (!history_) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "The command manager has no history engine."));
                return;
            }
            history_->redo(std::move(completion));
        }

        ProjectUndoEngine* history() const {
            return history_;
        }

        void beginStepTransaction() {
            if (begin_transaction_ && end_transaction_) {
                begin_transaction_();
                ++open_step_transactions_;
            }
        }

        void endStepTransaction() {
            if (open_step_transactions_ == 0)
                return;
            --open_step_transactions_;
            end_transaction_();
        }

    private:
        void applyWithoutRecording(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) {
            auto context = std::make_shared<CommandExecutionContext>(
                origin,
                origin == ProjectMutationOrigin::UndoRedo,
                dispatch_);
            auto transaction = command->batchesDocumentEvents()
                ? std::make_shared<ScopedTransaction>(begin_transaction_, end_transaction_)
                : nullptr;
            // The command is kept alive by the completion it was handed.
            auto held = command;
            held->execute(
                *context,
                [context, transaction, held, completion = std::move(completion)](
                    ProjectCommandResult result) mutable {
                    transaction.reset();
                    if (completion)
                        completion(std::move(result));
                });
        }

        static void complete(
            ProjectCommandCompletion completion,
            ProjectCommandResult result) {
            if (completion)
                completion(std::move(result));
        }

        ProjectUndoEngine* history_{};
        ProjectModelThreadDispatcher dispatch_{};
        std::function<void()> begin_transaction_{};
        std::function<void()> end_transaction_{};
        std::thread::id model_thread_id_{};
        uint32_t open_step_transactions_{0};
    };

    ProjectCommandManager::ProjectCommandManager(Configuration configuration)
        : impl_(std::make_shared<Impl>(std::move(configuration))) {
    }

    ProjectCommandManager::~ProjectCommandManager() = default;

    void ProjectCommandManager::execute(
        ProjectCommandPtr command,
        ProjectCommandCompletion completion) {
        impl_->execute(
            std::move(command),
            ProjectMutationOrigin::User,
            std::move(completion));
    }

    void ProjectCommandManager::execute(
        ProjectCommandPtr command,
        ProjectMutationOrigin origin,
        ProjectCommandCompletion completion) {
        impl_->execute(std::move(command), origin, std::move(completion));
    }

    ProjectCommandResult ProjectCommandManager::executeSynchronously(
        ProjectCommandPtr command,
        ProjectMutationOrigin origin) {
        return impl_->executeSynchronously(std::move(command), origin);
    }

    void ProjectCommandManager::undo(ProjectCommandCompletion completion) {
        impl_->undo(std::move(completion));
    }

    void ProjectCommandManager::redo(ProjectCommandCompletion completion) {
        impl_->redo(std::move(completion));
    }

    ProjectUndoState ProjectCommandManager::state() const {
        auto* history = impl_->history();
        return history ? history->state() : ProjectUndoState{};
    }

    ProjectUndoEngine& ProjectCommandManager::history() {
        return *impl_->history();
    }

    ProjectUndoResult ProjectCommandManager::beginStep(
        std::string description,
        ProjectMutationOrigin origin) {
        auto* history = impl_->history();
        if (!history)
            return ProjectUndoResult::failure("The command manager has no history engine.");
        auto result = history->beginCompound(std::move(description), origin);
        if (result.succeeded())
            impl_->beginStepTransaction();
        return result;
    }

    void ProjectCommandManager::endStep(ProjectCommandCompletion completion) {
        auto* history = impl_->history();
        if (!history) {
            if (completion)
                completion(ProjectCommandResult::failure(
                    "The command manager has no history engine."));
            return;
        }
        impl_->endStepTransaction();
        history->endCompound(std::move(completion));
    }

    void ProjectCommandManager::cancelStep(ProjectCommandCompletion completion) {
        auto* history = impl_->history();
        if (!history) {
            if (completion)
                completion(ProjectCommandResult::failure(
                    "The command manager has no history engine."));
            return;
        }
        impl_->endStepTransaction();
        history->cancelCompound(std::move(completion));
    }

    ProjectUndoResult ProjectCommandManager::beginGesture(
        std::string description,
        ProjectMutationOrigin origin) {
        auto* history = impl_->history();
        if (!history)
            return ProjectUndoResult::failure("The command manager has no history engine.");
        auto result = history->beginGesture(std::move(description), origin);
        if (result.succeeded())
            impl_->beginStepTransaction();
        return result;
    }

    void ProjectCommandManager::endGesture(ProjectCommandCompletion completion) {
        endStep(std::move(completion));
    }

    void ProjectCommandManager::cancelGesture(ProjectCommandCompletion completion) {
        cancelStep(std::move(completion));
    }

    void ProjectCommandManager::shutdown() {
        if (auto* history = impl_->history())
            history->shutdown();
    }

} // namespace uapmd
