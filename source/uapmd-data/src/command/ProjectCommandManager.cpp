#include "uapmd-data/uapmd-data.hpp"

#include "CommandExecution.hpp"

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
    } // namespace

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

            history_->executeCommand(
                std::move(command), origin, std::move(completion));
        }

        void recordExecuted(
            ProjectCommandPtr command,
            ProjectCommandPtr revert,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) {
            if (!command || !revert) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "Recording an executed change needs both directions."));
                return;
            }
            if (!history_) {
                complete(std::move(completion), ProjectCommandResult::failure(
                    "The command manager has no history engine."));
                return;
            }
            if (!recordsHistory(origin)) {
                // Nothing to apply and nothing to record.
                complete(std::move(completion), ProjectCommandResult::success());
                return;
            }
            history_->recordCommand(
                std::move(command), std::move(revert), origin, std::move(completion));
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

        ProjectHistory* history() const {
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
            auto context = std::make_shared<command_detail::CommandExecutionContext>(
                origin,
                origin == ProjectMutationOrigin::UndoRedo,
                dispatch_);
            auto transaction = command->batchesDocumentEvents()
                ? std::make_shared<command_detail::ScopedTransaction>(begin_transaction_, end_transaction_)
                : nullptr;
            // The command is kept alive by the completion it was handed.
            auto held = command;
            held->execute(
                *context,
                [context, transaction = std::move(transaction), held,
                 completion = std::move(completion)](
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

        ProjectHistory* history_{};
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

    void ProjectCommandManager::recordExecuted(
        ProjectCommandPtr command,
        ProjectCommandPtr revert,
        ProjectMutationOrigin origin,
        ProjectCommandCompletion completion) {
        impl_->recordExecuted(
            std::move(command),
            std::move(revert),
            origin,
            std::move(completion));
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

    bool ProjectCommandManager::markSaved() {
        auto* history = impl_->history();
        return history && history->markSaved();
    }

    bool ProjectCommandManager::markStateSaved(uint64_t stateId) {
        auto* history = impl_->history();
        return history && history->markStateSaved(stateId);
    }

    bool ProjectCommandManager::clear(bool markCurrentStateSaved) {
        auto* history = impl_->history();
        return history && history->clear(markCurrentStateSaved);
    }

    bool ProjectCommandManager::setMaximumHistorySizeInBytes(size_t value) {
        auto* history = impl_->history();
        return history && history->setMaximumHistorySizeInBytes(value);
    }

    ProjectUndoResult ProjectCommandManager::beginStep(
        std::string description,
        ProjectMutationOrigin origin,
        ProjectStepEventBatching batching) {
        auto* history = impl_->history();
        if (!history)
            return ProjectUndoResult::failure("The command manager has no history engine.");
        auto result = history->beginStep(std::move(description), origin);
        if (result.succeeded() && batching == ProjectStepEventBatching::WholeStep)
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
        history->endStep(std::move(completion));
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
        history->cancelStep(std::move(completion));
    }

    ProjectUndoResult ProjectCommandManager::beginGesture(
        std::string description,
        ProjectMutationOrigin origin,
        ProjectStepEventBatching batching) {
        auto* history = impl_->history();
        if (!history)
            return ProjectUndoResult::failure("The command manager has no history engine.");
        auto result = history->beginGesture(std::move(description), origin);
        if (result.succeeded() && batching == ProjectStepEventBatching::WholeStep)
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
