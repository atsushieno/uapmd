#pragma once

// Pieces shared by the command manager and the default history: how one
// command is run, and how the document events it produces are batched.
//
// Private to uapmd-data.

#include <functional>
#include <memory>
#include <utility>

#include "uapmd-data/uapmd-data.hpp"

namespace uapmd::command_detail {

    // Opens a document transaction for as long as it lives. Transactions
    // nest, so a command inside a step simply adds a level.
    //
    // Hold exactly one of these per execution. A synchronous command completes
    // while its caller is still on the stack, so a second owner would keep the
    // transaction open across whatever the completion goes on to do.
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

} // namespace uapmd::command_detail
