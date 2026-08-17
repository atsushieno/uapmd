#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "ProjectUndo.hpp"

namespace uapmd {

    class ProjectCommandContext;

    // The command layer reuses the history engine's result vocabulary rather
    // than introducing a parallel one. If commands become the primary
    // abstraction, rename ProjectUndoResult/ProjectUndoStatus instead of
    // keeping both names alive.
    using ProjectCommandResult = ProjectUndoResult;
    using ProjectCommandCompletion = std::function<void(ProjectCommandResult)>;

    class ProjectCommand;
    using ProjectCommandPtr = std::shared_ptr<ProjectCommand>;

    // One intended document change, expressed as a value.
    //
    // A command carries its whole payload -- the identities it addresses and
    // the values it writes -- as ordinary data members. It must NOT carry
    // std::function hooks that re-implement the mutation: that is what makes
    // an operation unreadable, unloggable, and impossible to construct
    // anywhere except at the one call site that knows how to bind it.
    //
    // A command binds to the *service* that owns the objects it mutates (a
    // reference held from construction, supplied by whoever creates it). That
    // is a service locator, not a re-implementation, and it keeps concrete
    // commands in the module that owns their targets while this interface
    // stays free of engine dependencies.
    class ProjectCommand {
    protected:
        ProjectCommand() = default;

    public:
        virtual ~ProjectCommand() = default;

        ProjectCommand(const ProjectCommand&) = delete;
        ProjectCommand& operator=(const ProjectCommand&) = delete;

        // Stable dotted identifier, for example "clip.setGain". Distinct from
        // description(): the id is machine-facing (logging, coalescing, and
        // eventually name-based dispatch from JavaScript and MCP), while the
        // description is the user-visible history label.
        virtual std::string_view commandId() const = 0;
        virtual std::string description() const = 0;

        // Bytes retained for as long as this command sits in history.
        virtual size_t retainedSizeInBytes() const = 0;

        // Coalescing of adjacent commands inside a gesture scope. Only ever
        // called with a command of the same commandId(), so implementations
        // may static_cast rather than dynamic_cast. Return true after adopting
        // `subsequent`'s payload, so that a drag records one history step
        // holding the initial and final values.
        virtual bool mergeWith(const ProjectCommand& subsequent) {
            return false;
        }

        // Whether the manager should wrap this execution in a document
        // transaction, so that observers see one batch of events.
        //
        // Commands that finish asynchronously across external work -- a
        // plug-in state request, an ARA archive -- must return false. A
        // document transaction must not be held open across such a gap: an
        // ARA plug-in cannot be archived while its document is being edited,
        // which is exactly what an open transaction represents. Those commands
        // batch their own events around the synchronous parts instead.
        virtual bool batchesDocumentEvents() const {
            return true;
        }

        // Performs the change. Implementations must invoke `completion`
        // exactly once; synchronous commands do so before returning.
        //
        // Before mutating, an implementation captures the state it is about to
        // overwrite and hands the manager the command that restores it, via
        // ProjectCommandContext::recordRevert(). A command that finds the
        // document already in the requested state simply completes without
        // recording a revert, and no history entry is created -- which
        // replaces the `if (before == after) return true;` early-out that
        // otherwise has to be written into every mutation entry point.
        virtual void execute(
            ProjectCommandContext& context,
            ProjectCommandCompletion completion) = 0;
    };

    // Supplied to every execution. Everything the manager needs to know about
    // an execution flows back through here, so that policy stays in the
    // manager instead of being restated by each command.
    class ProjectCommandContext {
    protected:
        ProjectCommandContext() = default;

    public:
        virtual ~ProjectCommandContext() = default;

        ProjectCommandContext(const ProjectCommandContext&) = delete;
        ProjectCommandContext& operator=(const ProjectCommandContext&) = delete;

        virtual ProjectMutationOrigin origin() const = 0;

        // True while this command is running as an undo or redo of an earlier
        // step. Commands rarely need it -- the manager already suppresses
        // history recording during replay -- but a command that drives
        // external state (a plug-in editor, an ARA document) may want to
        // distinguish a fresh edit from a replay.
        virtual bool replaying() const = 0;

        // Registers the command that reverses what this command is about to
        // do, built from state captured before mutating.
        //
        // The reverting command is expected to register the original command
        // as its own revert when it runs. That symmetry is what makes redo
        // work without a separate code path: undo executes the revert, which
        // re-registers the original, which redo then executes.
        //
        // Calling this more than once within a single execute() replaces the
        // previous registration; not calling it at all means "this execution
        // changed nothing".
        virtual void recordRevert(ProjectCommandPtr revert) = 0;

        // Marshals `task` onto the model thread. Asynchronous commands finish
        // on whichever thread completes their external work (a plug-in state
        // request, a file read); every document mutation and every call to
        // recordRevert() must be dispatched through here first.
        virtual void dispatchToModelThread(ProjectUndoTask task) = 0;
    };

} // namespace uapmd
