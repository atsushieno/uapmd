#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace remidy {

    class QueuedStateOperationManager {
        struct QueuedOperation {
            std::function<void()> start;
            std::function<void()> cancel;
        };

        struct SharedState {
            std::mutex mutex;
            std::deque<QueuedOperation> queue{};
            std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
            bool active{false};
            bool shuttingDown{false};
            std::string shutdownError{"instance destroyed"};
        };

        std::shared_ptr<SharedState> state_{std::make_shared<SharedState>()};

        static void startNext(const std::shared_ptr<SharedState>& state) {
            QueuedOperation next{};
            {
                std::lock_guard lock(state->mutex);
                if (state->queue.empty()) {
                    state->active = false;
                    return;
                }
                next = std::move(state->queue.front());
                state->queue.pop_front();
            }
            next.start();
        }

        static std::function<bool()> makeCancellationProbe(const std::weak_ptr<std::atomic<bool>>& cancelled) {
            return [cancelled]() -> bool {
                if (auto flag = cancelled.lock())
                    return flag->load(std::memory_order_acquire);
                return true;
            };
        }

        void enqueueOperation(QueuedOperation operation) {
            auto state = state_;
            bool shouldStartNow = false;
            bool shuttingDown = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->shuttingDown) {
                    shuttingDown = true;
                } else {
                    state->queue.push_back(std::move(operation));
                    if (!state->active) {
                        state->active = true;
                        shouldStartNow = true;
                    }
                }
            }

            if (!shouldStartNow) {
                if (shuttingDown)
                    operation.cancel();
                return;
            }

            startNext(state);
        }

    public:
        using RequestReceiver = std::function<void(std::vector<uint8_t> state, std::string error, void* callbackContext)>;
        using LoadCompleted = std::function<void(std::string error, void* callbackContext)>;
        using RequestStarter = std::function<void(std::function<bool()> isCancelled, std::function<void(std::vector<uint8_t> state, std::string error)> finish)>;
        using LoadStarter = std::function<void(std::function<bool()> isCancelled, std::function<void(std::string error)> finish)>;

        ~QueuedStateOperationManager() {
            shutdown("instance destroyed");
        }

        void enqueueRequest(void* callbackContext, RequestReceiver receiver, RequestStarter starter) {
            auto state = state_;
            auto cancelled = std::weak_ptr<std::atomic<bool>>(state->cancelled);
            auto sharedReceiver = std::make_shared<RequestReceiver>(std::move(receiver));
            enqueueOperation(QueuedOperation{
                    .start = [state, cancelled, callbackContext, receiver = sharedReceiver, starter = std::move(starter)]() mutable {
                        starter(makeCancellationProbe(cancelled),
                                [state, callbackContext, receiver](std::vector<uint8_t> responseState, std::string error) mutable {
                                    (*receiver)(std::move(responseState), std::move(error), callbackContext);
                                    startNext(state);
                                });
                    },
                    .cancel = [state, callbackContext, receiver = sharedReceiver]() mutable {
                        (*receiver)({}, state->shutdownError, callbackContext);
                    }
            });
        }

        void enqueueLoad(void* callbackContext, LoadCompleted completed, LoadStarter starter) {
            auto state = state_;
            auto cancelled = std::weak_ptr<std::atomic<bool>>(state->cancelled);
            auto sharedCompleted = std::make_shared<LoadCompleted>(std::move(completed));
            enqueueOperation(QueuedOperation{
                    .start = [state, cancelled, callbackContext, completed = sharedCompleted, starter = std::move(starter)]() mutable {
                        starter(makeCancellationProbe(cancelled),
                                [state, callbackContext, completed](std::string error) mutable {
                                    (*completed)(std::move(error), callbackContext);
                                    startNext(state);
                                });
                    },
                    .cancel = [state, callbackContext, completed = sharedCompleted]() mutable {
                        (*completed)(state->shutdownError, callbackContext);
                    }
            });
        }

        void shutdown(std::string error) {
            auto state = std::move(state_);
            if (!state)
                return;

            std::deque<QueuedOperation> pending{};
            {
                std::lock_guard lock(state->mutex);
                state->shuttingDown = true;
                state->shutdownError = std::move(error);
                state->cancelled->store(true, std::memory_order_release);
                pending.swap(state->queue);
            }

            for (auto& op : pending)
                op.cancel();
        }
    };

}
