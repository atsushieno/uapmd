#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>

#include "ProjectCommand.hpp"
#include "ProjectUndo.hpp"

namespace uapmd {

    // The history a project records its changes in.
    //
    // Deliberately expressed in commands, never in ProjectUndoableOperation:
    // an implementation that snapshots the document or journals intents has no
    // use for a retained inverse, and forcing one on it would make it emulate
    // the very model it exists to replace. A command, by contrast, is a value
    // with a stable id that every kind of history can consume -- store the
    // inverse it reports, snapshot around it, or write it to a log.
    //
    // Thread affinity: the public methods are called on the model thread.
    // Completions may be invoked from any thread and are marshalled back
    // before history state is touched.
    class ProjectHistory {
    public:
        virtual ~ProjectHistory() = default;

        ProjectHistory(const ProjectHistory&) = delete;
        ProjectHistory& operator=(const ProjectHistory&) = delete;

        // Runs `command` and records the step it reports. A command that
        // registers no revert changed nothing, and no entry is created.
        //
        // The execution happens here rather than in the caller so that an
        // implementation needing a pre-image -- a snapshot history -- can take
        // one before the document moves.
        virtual void executeCommand(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) = 0;

        // Records a change already applied outside the history, as the pair of
        // commands that move between the two states. `forward` is not run now.
        //
        // Some changes can only be recorded this way: the identity and state
        // of a newly constructed track or plug-in do not exist until after it
        // has been built, and capturing them is itself asynchronous.
        virtual void recordCommand(
            ProjectCommandPtr forward,
            ProjectCommandPtr revert,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) = 0;

        virtual void undo(ProjectCommandCompletion completion) = 0;
        virtual void redo(ProjectCommandCompletion completion) = 0;
        virtual ProjectUndoState state() const = 0;

        // One named step spanning several commands. Nested steps are rejected.
        virtual ProjectUndoResult beginStep(
            std::string description,
            ProjectMutationOrigin origin) = 0;
        virtual void endStep(ProjectCommandCompletion completion) = 0;
        // Reverts every child that already ran, in reverse order.
        virtual void cancelStep(ProjectCommandCompletion completion) = 0;

        // A step that coalesces adjacent commands sharing a commandId(), so a
        // drag records only the value it started from and the one it ended on.
        virtual ProjectUndoResult beginGesture(
            std::string description,
            ProjectMutationOrigin origin) = 0;
        virtual void endGesture(ProjectCommandCompletion completion) = 0;
        virtual void cancelGesture(ProjectCommandCompletion completion) = 0;

        // Save points. What "dirty" means belongs to the history: a numeric
        // cursor cannot tell an undo followed by a fresh edit from the state
        // that was saved.
        virtual bool markSaved() = 0;
        virtual bool markStateSaved(uint64_t stateId) = 0;

        // Releases everything retained and makes the current document a new
        // history root. Fails while a command is pending or a step is open.
        virtual bool clear(bool markCurrentStateSaved) = 0;
        virtual bool setMaximumHistorySizeInBytes(size_t value) = 0;

        // Refuses new work and completes pending client notification as
        // Cancelled. Commands own cancellation of their own external work.
        virtual void shutdown() = 0;

    protected:
        ProjectHistory() = default;
    };

    // What a history needs from the document it records for.
    //
    // Passed to a factory rather than baked into a configuration, because only
    // the document itself can supply these: batching its observer events and
    // getting work back onto its model thread.
    struct ProjectHistoryEnvironment {
        // Marshals work onto the model thread. Leave empty in single threaded
        // tests, where tasks run inline.
        ProjectModelThreadDispatcher dispatchToModelThread{};

        // Groups the document events one replayed command produces into a
        // single batch. Calls nest.
        std::function<void()> beginDocumentTransaction{};
        std::function<void()> endDocumentTransaction{};
    };

    // Chooses which history a project records into. An empty factory means the
    // default one.
    using ProjectHistoryFactory = std::function<
        std::unique_ptr<ProjectHistory>(const ProjectHistoryEnvironment&)>;

    // Builds a CommandInverseHistory with the standard retention budget.
    ProjectHistoryFactory defaultProjectHistoryFactory();

    // The default history: every step is a command and the command that
    // reverses it, replayed in either direction.
    //
    // Adapts command pairs onto ProjectUndoEngine, which remains the
    // operation-level engine underneath.
    class CommandInverseHistory final : public ProjectHistory {
    public:
        struct Configuration {
            size_t maximumHistorySizeInBytes{64u * 1024u * 1024u};
            ProjectHistoryEnvironment environment{};
        };

        CommandInverseHistory();
        explicit CommandInverseHistory(Configuration configuration);
        ~CommandInverseHistory() override;

        void executeCommand(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) override;
        void recordCommand(
            ProjectCommandPtr forward,
            ProjectCommandPtr revert,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) override;

        void undo(ProjectCommandCompletion completion) override;
        void redo(ProjectCommandCompletion completion) override;
        ProjectUndoState state() const override;

        ProjectUndoResult beginStep(
            std::string description, ProjectMutationOrigin origin) override;
        void endStep(ProjectCommandCompletion completion) override;
        void cancelStep(ProjectCommandCompletion completion) override;
        ProjectUndoResult beginGesture(
            std::string description, ProjectMutationOrigin origin) override;
        void endGesture(ProjectCommandCompletion completion) override;
        void cancelGesture(ProjectCommandCompletion completion) override;

        bool markSaved() override;
        bool markStateSaved(uint64_t stateId) override;
        bool clear(bool markCurrentStateSaved) override;
        bool setMaximumHistorySizeInBytes(size_t value) override;
        void shutdown() override;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

    // A history that records nothing.
    //
    // Commands still run and the document still changes; only the recording is
    // dropped. For contexts where undo is meaningless -- offline rendering, a
    // track freeze, a headless tool -- and as the check that this interface
    // really is expressed in commands rather than in one implementation's
    // internals.
    //
    // It reports the project as never dirty, because without history there is
    // nothing to compare the current state against. A caller that decides
    // whether to prompt for saving must not be given one of these.
    class NullProjectHistory final : public ProjectHistory {
    public:
        struct Configuration {
            ProjectHistoryEnvironment environment{};
        };

        NullProjectHistory();
        explicit NullProjectHistory(Configuration configuration);
        ~NullProjectHistory() override;

        void executeCommand(
            ProjectCommandPtr command,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) override;
        void recordCommand(
            ProjectCommandPtr forward,
            ProjectCommandPtr revert,
            ProjectMutationOrigin origin,
            ProjectCommandCompletion completion) override;

        void undo(ProjectCommandCompletion completion) override;
        void redo(ProjectCommandCompletion completion) override;
        ProjectUndoState state() const override;

        ProjectUndoResult beginStep(
            std::string description, ProjectMutationOrigin origin) override;
        void endStep(ProjectCommandCompletion completion) override;
        void cancelStep(ProjectCommandCompletion completion) override;
        ProjectUndoResult beginGesture(
            std::string description, ProjectMutationOrigin origin) override;
        void endGesture(ProjectCommandCompletion completion) override;
        void cancelGesture(ProjectCommandCompletion completion) override;

        bool markSaved() override;
        bool markStateSaved(uint64_t stateId) override;
        bool clear(bool markCurrentStateSaved) override;
        bool setMaximumHistorySizeInBytes(size_t value) override;
        void shutdown() override;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace uapmd
