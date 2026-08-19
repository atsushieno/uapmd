#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ProjectCommand.hpp"
#include "ProjectUndo.hpp"

namespace uapmd {

    // Whether one history step delivers its document events as a single batch.
    //
    // This is the step-level counterpart of
    // ProjectCommand::batchesDocumentEvents(), and it defaults to the safe
    // answer for the same reason: a document transaction must not be held open
    // across external asynchronous work. A step that removes tracks, imports
    // stems, or stays open across remote round trips spans exactly such gaps --
    // an ARA plug-in cannot be archived while its document is being edited,
    // which is what an open transaction represents -- and observers that only
    // learn of the change at the end of the step read stale document state in
    // the meantime.
    enum class ProjectStepEventBatching {
        // Each command in the step batches its own events. Observers see the
        // step progress, which is what a multi-stage or asynchronous action
        // needs.
        PerCommand,
        // One batch of events for the whole step, so observers never see the
        // action half-applied. Only for a step whose commands all run inline.
        WholeStep,
    };

    // The single entry point for changing the project document.
    //
    // Everything that a mutation entry point currently restates for itself
    // lives here instead:
    //
    //   * origin policy -- User and Remote edits enter history; Load,
    //     UndoRedo and Internal ones are applied without recording. Today that
    //     decision is copy-pasted into every undoable method.
    //   * document transaction scoping -- one batch of document events per
    //     top-level command, so observers never see a change half-applied.
    //     Today each caller opens its own scope, or forgets to.
    //   * history interaction -- constructing the history entry from the
    //     command and the revert it recorded.
    //   * model-thread affinity and completion marshalling for asynchronous
    //     commands.
    //
    // Mutation services (TimelineFacade and friends) keep their primitive
    // "apply the change and emit the event" methods, and lose their
    // ProjectMutationOrigin parameters: origin is a manager concern, not
    // something 41 signatures need to carry.
    class ProjectCommandManager {
    public:
        struct Configuration {
            // The history this manager records into. Required, and referenced
            // rather than owned: during migration the same history is still
            // driven directly by operations that have not become commands yet,
            // and two histories would silently diverge.
            ProjectUndoEngine* history{};

            // Marshals work onto the model thread. Leave empty in single
            // threaded tests, where tasks run inline.
            ProjectModelThreadDispatcher dispatchToModelThread{};

            // Groups the document events produced by one command, or one step,
            // into a single batch. Supplied by the owner rather than called
            // directly, so that this layer stays independent of the timeline.
            // Calls nest.
            std::function<void()> beginDocumentTransaction{};
            std::function<void()> endDocumentTransaction{};
        };

        explicit ProjectCommandManager(Configuration configuration);
        ~ProjectCommandManager();

        ProjectCommandManager(const ProjectCommandManager&) = delete;
        ProjectCommandManager& operator=(const ProjectCommandManager&) = delete;

        // Executes one command. `completion` is invoked exactly once, on the
        // model thread.
        void execute(
            ProjectCommandPtr command,
            ProjectCommandCompletion completion = {});
        void execute(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion = {});

        // Records a change whose forward mutation has already happened -- a
        // plug-in that moved its own parameter, or an object whose state could
        // only be captured after it was constructed.
        //
        // `command` is not executed now: it becomes the redo, and `revert` the
        // undo. Both keep their ordinary meanings from then on.
        void recordExecuted(
            ProjectCommandPtr command,
            ProjectCommandPtr revert,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectCommandCompletion completion = {});

        // For commands that are genuinely inline, such as a clip property
        // edit. Must be called on the model thread; called from anywhere else
        // it returns Busy rather than pretending the command failed.
        //
        // This replaces the shared_ptr<optional<ProjectUndoResult>> idiom that
        // the current undoable-property helpers use to fake a synchronous
        // return out of an asynchronous engine.
        ProjectCommandResult executeSynchronously(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User);

        void undo(ProjectCommandCompletion completion = {});
        void redo(ProjectCommandCompletion completion = {});

        ProjectUndoState state() const;

        // Save points and retention. These belong to the history contract
        // rather than to whoever happens to write the file: what "dirty" means
        // is decided by the history, and a caller that reached past this layer
        // to the engine would keep working only for as long as there is
        // exactly one history implementation.
        //
        // Saving captures state().currentStateId when serialization starts and
        // marks that node saved once the write succeeds, so an edit made while
        // the write is in flight still leaves the project dirty.
        bool markSaved();
        bool markStateSaved(uint64_t stateId);
        // Releases every retained revert and establishes the current document
        // as a new history root. Fails while a command is pending or a step is
        // open.
        bool clear(bool markCurrentStateSaved = true);
        bool setMaximumHistorySizeInBytes(size_t value);

        // The history engine remains directly reachable while operations that
        // are not yet commands still record into it. New code should not need
        // it.
        ProjectUndoEngine& history();

        // One named history step spanning several commands. Prefer the scoped
        // forms below.
        ProjectUndoResult beginStep(
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectStepEventBatching batching = ProjectStepEventBatching::PerCommand);
        void endStep(ProjectCommandCompletion completion = {});
        void cancelStep(ProjectCommandCompletion completion = {});

        // A gesture is a step that coalesces adjacent commands sharing a
        // commandId(): intermediate values are applied, but history keeps only
        // the initial and final ones.
        //
        // WholeStep batching is almost never right here: a gesture lasts as
        // long as the user drags, and batching its events would freeze every
        // observer for the duration of the drag.
        ProjectUndoResult beginGesture(
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectStepEventBatching batching = ProjectStepEventBatching::PerCommand);
        void endGesture(ProjectCommandCompletion completion = {});
        void cancelGesture(ProjectCommandCompletion completion = {});

        void shutdown();

    private:
        class Impl;
        std::shared_ptr<Impl> impl_;
    };

    // Declare one for a user action that performs several commands. Replaces
    // the hand-paired beginCompound()/endCompound() calls that currently sit
    // in the GUI, the application model, and the JavaScript runtime.
    //
    // Destroying the scope without commit() cancels the step, so an early
    // return or a failed command cannot leave a half-finished step open.
    class ScopedCommandStep {
        ProjectCommandManager* manager_;
        bool open_;

    public:
        ScopedCommandStep(
            ProjectCommandManager& manager,
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectStepEventBatching batching = ProjectStepEventBatching::PerCommand)
            : manager_(&manager)
            , open_(manager.beginStep(std::move(description), origin, batching).succeeded()) {
        }

        ~ScopedCommandStep() {
            if (open_)
                manager_->cancelStep();
        }

        ScopedCommandStep(const ScopedCommandStep&) = delete;
        ScopedCommandStep& operator=(const ScopedCommandStep&) = delete;

        // False when a step was already open; nested steps are rejected, and
        // the caller decides whether that is fatal or simply means it is
        // already inside a larger action.
        bool opened() const {
            return open_;
        }

        void commit(ProjectCommandCompletion completion = {}) {
            if (!open_)
                return;
            open_ = false;
            manager_->endStep(std::move(completion));
        }
    };

    class ScopedCommandGesture {
        ProjectCommandManager* manager_;
        bool open_;

    public:
        ScopedCommandGesture(
            ProjectCommandManager& manager,
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectStepEventBatching batching = ProjectStepEventBatching::PerCommand)
            : manager_(&manager)
            , open_(manager.beginGesture(std::move(description), origin, batching).succeeded()) {
        }

        ~ScopedCommandGesture() {
            if (open_)
                manager_->endGesture();
        }

        ScopedCommandGesture(const ScopedCommandGesture&) = delete;
        ScopedCommandGesture& operator=(const ScopedCommandGesture&) = delete;

        bool opened() const {
            return open_;
        }
    };

} // namespace uapmd
