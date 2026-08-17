#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace uapmd {

    enum class ProjectMutationOrigin {
        User,
        UndoRedo,
        Load,
        Remote,
        Internal
    };

    enum class ProjectUndoStatus {
        Succeeded,
        Busy,
        NothingToUndo,
        NothingToRedo,
        Failed,
        Cancelled,
        Stopped
    };

    struct ProjectUndoResult {
        ProjectUndoStatus status{ProjectUndoStatus::Succeeded};
        std::string error{};

        bool succeeded() const {
            return status == ProjectUndoStatus::Succeeded;
        }

        static ProjectUndoResult success() {
            return {};
        }

        static ProjectUndoResult failure(std::string message) {
            return {
                .status = ProjectUndoStatus::Failed,
                .error = std::move(message)
            };
        }
    };

    using ProjectUndoTask = std::function<void()>;
    using ProjectUndoCompletion = std::function<void(ProjectUndoResult)>;
    using ProjectModelThreadDispatcher = std::function<void(ProjectUndoTask)>;

    // Passed to every operation invocation. Operations may finish on any
    // thread, but all document commits must be dispatched through
    // dispatchToModelThread so that document and history state stay serialized.
    struct ProjectUndoExecutionContext {
        ProjectMutationOrigin origin{ProjectMutationOrigin::User};
        bool recordHistory{true};
        ProjectModelThreadDispatcher dispatchToModelThread{};

        void dispatch(ProjectUndoTask task) const {
            if (dispatchToModelThread)
                dispatchToModelThread(std::move(task));
            else if (task)
                task();
        }
    };

    // An operation owns everything needed to move one document edit in either
    // direction. Implementations must invoke their completion exactly once.
    // Synchronous operations invoke it before returning; plug-in and track
    // operations may invoke it later.
    class ProjectUndoableOperation {
    public:
        virtual ~ProjectUndoableOperation() = default;

        virtual std::string description() const = 0;
        virtual size_t historySizeInBytes() const = 0;

        // Called only for already-performed adjacent operations inside an
        // explicit gesture scope. Implementations return true after extending
        // this operation to include `subsequent`.
        virtual bool mergeWith(const ProjectUndoableOperation& subsequent) {
            return false;
        }
        virtual bool hasEffect() const {
            return true;
        }

        virtual void perform(
            const ProjectUndoExecutionContext& context,
            ProjectUndoCompletion completion) = 0;
        virtual void undo(
            const ProjectUndoExecutionContext& context,
            ProjectUndoCompletion completion) = 0;
        virtual void redo(
            const ProjectUndoExecutionContext& context,
            ProjectUndoCompletion completion) = 0;
    };

    struct ProjectUndoState {
        bool busy{false};
        bool compoundOpen{false};
        bool gestureOpen{false};
        bool canUndo{false};
        bool canRedo{false};
        bool dirty{false};
        std::string compoundDescription{};
        std::string undoDescription{};
        std::string redoDescription{};
        size_t historySizeInBytes{0};
        size_t maximumHistorySizeInBytes{0};
        uint64_t currentStateId{0};
        uint64_t savedStateId{0};
    };

    // Thread-affine asynchronous history. Its public methods are called on the
    // model thread. Operation completions are accepted from any thread and are
    // marshalled back through Configuration::dispatchToModelThread before the
    // stacks or cursor are touched.
    class ProjectUndoEngine {
    public:
        struct Configuration {
            size_t maximumHistorySizeInBytes{64u * 1024u * 1024u};
            ProjectModelThreadDispatcher dispatchToModelThread{};
        };

        ProjectUndoEngine();
        explicit ProjectUndoEngine(Configuration configuration);
        ~ProjectUndoEngine();

        ProjectUndoEngine(const ProjectUndoEngine&) = delete;
        ProjectUndoEngine& operator=(const ProjectUndoEngine&) = delete;

        ProjectUndoState state() const;

        void perform(
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectUndoCompletion completion = {});
        void perform(
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectMutationOrigin origin,
            ProjectUndoCompletion completion = {});
        // Records an operation whose forward mutation has already completed.
        // This is for creation paths where the resulting persistent object and
        // its extension-owned state can only be captured after construction.
        // The operation's perform() is not invoked; undo() and redo() retain
        // their ordinary meanings.
        void recordPerformed(
            std::shared_ptr<ProjectUndoableOperation> operation,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User,
            ProjectUndoCompletion completion = {});
        void undo(ProjectUndoCompletion completion = {});
        void redo(ProjectUndoCompletion completion = {});

        // Opens one named history step. Operations performed while it is open
        // are applied immediately but enter history only when endCompound()
        // succeeds. Nested compounds are deliberately rejected.
        ProjectUndoResult beginCompound(
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User);
        void endCompound(ProjectUndoCompletion completion = {});
        // Reverts every successfully performed child in reverse order. If
        // compensation fails, the compound remains open for explicit recovery.
        void cancelCompound(ProjectUndoCompletion completion = {});

        // A gesture is a named compound scope that coalesces adjacent,
        // compatible operations. Intermediate values are applied normally,
        // while history retains only the initial and final values.
        ProjectUndoResult beginGesture(
            std::string description,
            ProjectMutationOrigin origin = ProjectMutationOrigin::User);
        void endGesture(ProjectUndoCompletion completion = {});
        void cancelGesture(ProjectUndoCompletion completion = {});

        // Fails while an operation is pending or a compound step is open.
        // Clearing releases all retained fragments and establishes the current
        // document as a new history root.
        bool clear(bool markCurrentStateSaved = true);
        bool markSaved();
        bool markStateSaved(uint64_t stateId);
        bool setMaximumHistorySizeInBytes(size_t value);

        // Rejects new work and completes pending client notification as
        // Cancelled. The operation itself owns cancellation of external work.
        void shutdown();

    private:
        class Impl;
        std::shared_ptr<Impl> impl_;
    };

} // namespace uapmd
