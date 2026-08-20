#include "uapmd-data/uapmd-data.hpp"

#include <utility>

#include "CommandExecution.hpp"

namespace uapmd {

    namespace {

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

            // For a change that has already been applied: the revert is
            // supplied rather than captured by running the forward command.
            CommandOperation(
                ProjectCommandPtr command,
                ProjectCommandPtr revert,
                ProjectModelThreadDispatcher dispatch,
                std::function<void()> beginTransaction,
                std::function<void()> endTransaction)
                : forward_(std::move(command))
                , backward_(std::move(revert))
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
                auto context = std::make_shared<command_detail::CommandExecutionContext>(
                    engineContext.origin,
                    !engineContext.recordHistory,
                    dispatch_);
                auto transaction = command->batchesDocumentEvents()
                    ? std::make_shared<command_detail::ScopedTransaction>(
                        begin_transaction_, end_transaction_)
                    : nullptr;
                auto self = shared_from_this();
                auto* target = &opposite;
                command->execute(
                    *context,
                    // The transaction is moved in, not copied: a synchronous
                    // command completes while run() is still on the stack, so
                    // a second owner here would hold the transaction open
                    // across whatever the completion goes on to do -- the next
                    // child of a compound step, for one, which would then run
                    // inside a transaction it never opened.
                    [self, context, transaction = std::move(transaction), target,
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

    } // namespace

    class CommandInverseHistory::Impl {
    public:
        explicit Impl(Configuration configuration)
            : engine_({
                .maximumHistorySizeInBytes = configuration.maximumHistorySizeInBytes,
                .dispatchToModelThread = configuration.environment.dispatchToModelThread
            })
            , dispatch_(std::move(configuration.environment.dispatchToModelThread))
            , begin_transaction_(std::move(configuration.environment.beginDocumentTransaction))
            , end_transaction_(std::move(configuration.environment.endDocumentTransaction)) {
        }

        std::shared_ptr<CommandOperation> adapt(ProjectCommandPtr command) {
            return std::make_shared<CommandOperation>(
                std::move(command), dispatch_, begin_transaction_, end_transaction_);
        }

        std::shared_ptr<CommandOperation> adapt(
            ProjectCommandPtr command, ProjectCommandPtr revert) {
            return std::make_shared<CommandOperation>(
                std::move(command), std::move(revert),
                dispatch_, begin_transaction_, end_transaction_);
        }

        ProjectUndoEngine& engine() {
            return engine_;
        }

        const ProjectUndoEngine& engine() const {
            return engine_;
        }

    private:
        ProjectUndoEngine engine_;
        ProjectModelThreadDispatcher dispatch_{};
        std::function<void()> begin_transaction_{};
        std::function<void()> end_transaction_{};
    };

    CommandInverseHistory::CommandInverseHistory()
        : CommandInverseHistory(Configuration{}) {
    }

    CommandInverseHistory::CommandInverseHistory(Configuration configuration)
        : impl_(std::make_unique<Impl>(std::move(configuration))) {
    }

    CommandInverseHistory::~CommandInverseHistory() = default;

    void CommandInverseHistory::executeCommand(
        ProjectCommandPtr command,
        ProjectMutationOrigin origin,
        ProjectCommandCompletion completion) {
        impl_->engine().perform(
            impl_->adapt(std::move(command)), origin, std::move(completion));
    }

    void CommandInverseHistory::recordCommand(
        ProjectCommandPtr forward,
        ProjectCommandPtr revert,
        ProjectMutationOrigin origin,
        ProjectCommandCompletion completion) {
        impl_->engine().recordPerformed(
            impl_->adapt(std::move(forward), std::move(revert)),
            origin,
            std::move(completion));
    }

    void CommandInverseHistory::undo(ProjectCommandCompletion completion) {
        impl_->engine().undo(std::move(completion));
    }

    void CommandInverseHistory::redo(ProjectCommandCompletion completion) {
        impl_->engine().redo(std::move(completion));
    }

    ProjectUndoState CommandInverseHistory::state() const {
        return impl_->engine().state();
    }

    ProjectUndoResult CommandInverseHistory::beginStep(
        std::string description, ProjectMutationOrigin origin) {
        return impl_->engine().beginCompound(std::move(description), origin);
    }

    void CommandInverseHistory::endStep(ProjectCommandCompletion completion) {
        impl_->engine().endCompound(std::move(completion));
    }

    void CommandInverseHistory::cancelStep(ProjectCommandCompletion completion) {
        impl_->engine().cancelCompound(std::move(completion));
    }

    ProjectUndoResult CommandInverseHistory::beginGesture(
        std::string description, ProjectMutationOrigin origin) {
        return impl_->engine().beginGesture(std::move(description), origin);
    }

    void CommandInverseHistory::endGesture(ProjectCommandCompletion completion) {
        impl_->engine().endGesture(std::move(completion));
    }

    void CommandInverseHistory::cancelGesture(ProjectCommandCompletion completion) {
        impl_->engine().cancelGesture(std::move(completion));
    }

    bool CommandInverseHistory::markSaved() {
        return impl_->engine().markSaved();
    }

    bool CommandInverseHistory::markStateSaved(uint64_t stateId) {
        return impl_->engine().markStateSaved(stateId);
    }

    bool CommandInverseHistory::clear(bool markCurrentStateSaved) {
        return impl_->engine().clear(markCurrentStateSaved);
    }

    bool CommandInverseHistory::setMaximumHistorySizeInBytes(size_t value) {
        return impl_->engine().setMaximumHistorySizeInBytes(value);
    }

    void CommandInverseHistory::shutdown() {
        impl_->engine().shutdown();
    }

    ProjectHistoryFactory defaultProjectHistoryFactory() {
        return [](const ProjectHistoryEnvironment& environment) {
            return std::make_unique<CommandInverseHistory>(
                CommandInverseHistory::Configuration{.environment = environment});
        };
    }

    class NullProjectHistory::Impl {
    public:
        explicit Impl(Configuration configuration)
            : dispatch_(std::move(configuration.environment.dispatchToModelThread))
            , begin_transaction_(std::move(configuration.environment.beginDocumentTransaction))
            , end_transaction_(std::move(configuration.environment.endDocumentTransaction)) {
        }

        // The change still has to happen; only the record of it is dropped.
        // The revert the command registers is simply discarded.
        void run(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) {
            if (!command) {
                if (completion)
                    completion(ProjectCommandResult::failure(
                        "Cannot execute an empty command."));
                return;
            }
            auto context = std::make_shared<command_detail::CommandExecutionContext>(
                origin, false, dispatch_);
            auto transaction = command->batchesDocumentEvents()
                ? std::make_shared<command_detail::ScopedTransaction>(
                    begin_transaction_, end_transaction_)
                : nullptr;
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

    private:
        ProjectModelThreadDispatcher dispatch_{};
        std::function<void()> begin_transaction_{};
        std::function<void()> end_transaction_{};
    };

    NullProjectHistory::NullProjectHistory()
        : NullProjectHistory(Configuration{}) {
    }

    NullProjectHistory::NullProjectHistory(Configuration configuration)
        : impl_(std::make_unique<Impl>(std::move(configuration))) {
    }

    NullProjectHistory::~NullProjectHistory() = default;

    void NullProjectHistory::executeCommand(
        ProjectCommandPtr command,
        ProjectMutationOrigin origin,
        ProjectCommandCompletion completion) {
        impl_->run(std::move(command), origin, std::move(completion));
    }

    void NullProjectHistory::recordCommand(
        ProjectCommandPtr,
        ProjectCommandPtr,
        ProjectMutationOrigin,
        ProjectCommandCompletion completion) {
        // The change has already been applied; there is nothing left to do.
        if (completion)
            completion(ProjectCommandResult::success());
    }

    void NullProjectHistory::undo(ProjectCommandCompletion completion) {
        if (completion)
            completion({.status = ProjectUndoStatus::NothingToUndo});
    }

    void NullProjectHistory::redo(ProjectCommandCompletion completion) {
        if (completion)
            completion({.status = ProjectUndoStatus::NothingToRedo});
    }

    ProjectUndoState NullProjectHistory::state() const {
        return {};
    }

    ProjectUndoResult NullProjectHistory::beginStep(
        std::string, ProjectMutationOrigin) {
        // Scopes are accepted so that callers which group several commands
        // work unchanged; grouping nothing is still nothing.
        return ProjectUndoResult::success();
    }

    void NullProjectHistory::endStep(ProjectCommandCompletion completion) {
        if (completion)
            completion(ProjectCommandResult::success());
    }

    void NullProjectHistory::cancelStep(ProjectCommandCompletion completion) {
        // Nothing was recorded, so nothing can be rolled back. A caller that
        // needs its commands undone on failure must compensate for itself.
        if (completion)
            completion(ProjectCommandResult::success());
    }

    ProjectUndoResult NullProjectHistory::beginGesture(
        std::string description, ProjectMutationOrigin origin) {
        return beginStep(std::move(description), origin);
    }

    void NullProjectHistory::endGesture(ProjectCommandCompletion completion) {
        endStep(std::move(completion));
    }

    void NullProjectHistory::cancelGesture(ProjectCommandCompletion completion) {
        cancelStep(std::move(completion));
    }

    bool NullProjectHistory::markSaved() {
        return true;
    }

    bool NullProjectHistory::markStateSaved(uint64_t) {
        return true;
    }

    bool NullProjectHistory::clear(bool) {
        return true;
    }

    bool NullProjectHistory::setMaximumHistorySizeInBytes(size_t) {
        return true;
    }

    void NullProjectHistory::shutdown() {
    }

} // namespace uapmd
