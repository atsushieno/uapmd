#if defined(__ANDROID__)

#include "AndroidPerformanceHintSession.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include "remidy/remidy.hpp"

namespace uapmd {

    namespace {
        // Portion of the callback period the audio thread may spend, leaving headroom for scheduling jitter.
        constexpr double kBudgetRatio = 0.9;
        constexpr double kSmoothing = 0.1;
        constexpr auto kOpenTimeout = std::chrono::seconds(2);

        // Resolved at runtime so that it works regardless of minSdk; notifyWorkload*() (API 36) are not in the NDK headers yet.
        struct PerformanceHintApi {
            void* (*getManager)(){nullptr};
            void* (*createSession)(void* manager, const int32_t* threadIds, size_t size, int64_t initialTargetNanos){nullptr};
            int (*updateTargetWorkDuration)(void* session, int64_t targetNanos){nullptr};
            int (*reportActualWorkDuration)(void* session, int64_t actualNanos){nullptr};
            void (*closeSession)(void* session){nullptr};
            int (*setThreads)(void* session, const pid_t* threadIds, size_t size){nullptr};
            int (*notifyWorkloadIncrease)(void* session, bool cpu, bool gpu, const char* debugName){nullptr};
            int (*notifyWorkloadReset)(void* session, bool cpu, bool gpu, const char* debugName){nullptr};
            int (*notifyWorkloadSpike)(void* session, bool cpu, bool gpu, const char* debugName){nullptr};
            void* manager{nullptr};

            PerformanceHintApi() {
                auto lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NODELETE);
                if (!lib)
                    return;
                getManager = reinterpret_cast<decltype(getManager)>(dlsym(lib, "APerformanceHint_getManager"));
                createSession = reinterpret_cast<decltype(createSession)>(dlsym(lib, "APerformanceHint_createSession"));
                updateTargetWorkDuration = reinterpret_cast<decltype(updateTargetWorkDuration)>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
                reportActualWorkDuration = reinterpret_cast<decltype(reportActualWorkDuration)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
                closeSession = reinterpret_cast<decltype(closeSession)>(dlsym(lib, "APerformanceHint_closeSession"));
                setThreads = reinterpret_cast<decltype(setThreads)>(dlsym(lib, "APerformanceHint_setThreads"));
                notifyWorkloadIncrease = reinterpret_cast<decltype(notifyWorkloadIncrease)>(dlsym(lib, "APerformanceHint_notifyWorkloadIncrease"));
                notifyWorkloadReset = reinterpret_cast<decltype(notifyWorkloadReset)>(dlsym(lib, "APerformanceHint_notifyWorkloadReset"));
                notifyWorkloadSpike = reinterpret_cast<decltype(notifyWorkloadSpike)>(dlsym(lib, "APerformanceHint_notifyWorkloadSpike"));
                if (getManager && createSession && updateTargetWorkDuration && reportActualWorkDuration && closeSession)
                    manager = getManager();
            }

            bool available() const { return manager != nullptr; }

            static PerformanceHintApi& get() {
                static PerformanceHintApi api{};
                return api;
            }
        };
    }

    AndroidPerformanceHintSession::~AndroidPerformanceHintSession() {
        stop();
    }

    void AndroidPerformanceHintSession::start(int64_t initialTargetNanos) {
        stop();
        if (initialTargetNanos <= 0 || !PerformanceHintApi::get().available())
            return;
        cancel_open_.store(false);
        callback_thread_.store(0);
        workload_serial_ = remidy::PerformanceHintCoordinator::workloadSerial();
        remote_round_trip_average_nanos_ = 0;
        remote_budget_scale_ = 0;
        // Creating a session is a system service call, so it happens off the audio thread once the callback thread is known.
        opener_ = std::thread([this, initialTargetNanos] {
            const auto deadline = std::chrono::steady_clock::now() + kOpenTimeout;
            pid_t tid = 0;
            while (!cancel_open_.load() && (tid = callback_thread_.load()) == 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (tid == 0 || cancel_open_.load())
                return;
            auto& api = PerformanceHintApi::get();
            int32_t threads[] = {tid};
            auto session = api.createSession(api.manager, threads, 1, initialTargetNanos);
            if (!session)
                return;
            session_thread_ = tid;
            session_target_nanos_ = initialTargetNanos;
            session_.store(session, std::memory_order_release);
        });
    }

    void AndroidPerformanceHintSession::stop() {
        cancel_open_.store(true);
        if (opener_.joinable())
            opener_.join();
        if (auto session = session_.exchange(nullptr))
            PerformanceHintApi::get().closeSession(session);
        remidy::PerformanceHintCoordinator::remoteBudgetScale(0.0f);
    }

    void AndroidPerformanceHintSession::beginCallback() {
        if (callback_thread_.load(std::memory_order_relaxed) == 0)
            callback_thread_.store(gettid(), std::memory_order_relaxed);
    }

    void AndroidPerformanceHintSession::endCallback(int64_t elapsedNanos, int64_t periodNanos) {
        const auto remote = remidy::PerformanceHintCoordinator::takeRemoteProcessing();
        auto session = session_.load(std::memory_order_acquire);
        if (!session || periodNanos <= 0)
            return;
        auto& api = PerformanceHintApi::get();
        const auto budget = static_cast<double>(periodNanos) * kBudgetRatio;

        // Distribute the remaining slack to remote instances in proportion to their processing time.
        if (remote.processing_nanos > 0) {
            const auto scale = std::clamp(1.0 + (budget - static_cast<double>(elapsedNanos)) / static_cast<double>(remote.processing_nanos), 0.25, 4.0);
            remote_budget_scale_ = remote_budget_scale_ == 0 ? scale : remote_budget_scale_ * (1.0 - kSmoothing) + scale * kSmoothing;
            remidy::PerformanceHintCoordinator::remoteBudgetScale(static_cast<float>(remote_budget_scale_));
        }

        // The stream may restart on another thread after an error.
        if (const auto tid = gettid(); tid != session_thread_ && api.setThreads) {
            api.setThreads(session, &tid, 1);
            session_thread_ = tid;
        }

        if (const auto serial = remidy::PerformanceHintCoordinator::workloadSerial(); serial != workload_serial_) {
            workload_serial_ = serial;
            switch (remidy::PerformanceHintCoordinator::workload()) {
                case remidy::PerformanceWorkloadHint::Increase:
                    if (api.notifyWorkloadIncrease)
                        api.notifyWorkloadIncrease(session, true, false, "uapmd-workload-increase");
                    break;
                case remidy::PerformanceWorkloadHint::Spike:
                    if (api.notifyWorkloadSpike)
                        api.notifyWorkloadSpike(session, true, false, "uapmd-workload-spike");
                    break;
                case remidy::PerformanceWorkloadHint::Reset:
                    if (api.notifyWorkloadReset)
                        api.notifyWorkloadReset(session, true, false, "uapmd-workload-reset");
                    break;
                default:
                    break;
            }
        }

        // The host's own budget is what remains after waiting on out-of-process plugins.
        remote_round_trip_average_nanos_ = remote_round_trip_average_nanos_ * (1.0 - kSmoothing) + static_cast<double>(remote.round_trip_nanos) * kSmoothing;
        const auto target = std::max<int64_t>(periodNanos / 10, static_cast<int64_t>(budget - remote_round_trip_average_nanos_));
        if (std::llabs(target - session_target_nanos_) * 10 > session_target_nanos_ && api.updateTargetWorkDuration(session, target) == 0)
            session_target_nanos_ = target;

        api.reportActualWorkDuration(session, std::max<int64_t>(1, elapsedNanos - remote.round_trip_nanos));
    }
}

#endif // __ANDROID__
