#pragma once

#include <atomic>
#include <exception>
#include <clap/helpers/host.hh>
#include <clap/helpers/host.hxx>


#include "clap/events.h"

using CLAPHelperHost = clap::helpers::Host<
    clap::helpers::MisbehaviourHandler::Ignore,
    clap::helpers::CheckingLevel::Maximal
>;

namespace remidy {
    class PluginInstanceCLAP;

    class RemidyCLAPHost : public CLAPHelperHost {
        std::atomic<PluginInstanceCLAP*> attached_instance{nullptr};
        struct Timer {
            clap_id id{0};
            uint32_t periodMs{0};
            std::atomic<bool> running{false};
            std::thread worker{};
            std::mutex cvMutex{};
            std::condition_variable cv{};
        };
        std::mutex timersMutex{};
        std::unordered_map<clap_id, std::unique_ptr<Timer>> timers_{};
        std::atomic<clap_id> nextTimerId{1};
    protected:
        void requestRestart() noexcept override;

        void requestProcess() noexcept override;

        void requestCallback() noexcept override;

        // clap_host_params
        bool implementsParams() const noexcept override { return true; }
        void paramsRescan(clap_param_rescan_flags flags) noexcept override;
        void paramsClear(clap_id paramId, clap_param_clear_flags flags) noexcept override;
        void paramsRequestFlush() noexcept override;

    public:
        RemidyCLAPHost(
            const char* name = "remidy",
            const char* url = "",
            const char* vendor = "",
            const char* version = ""
        ) : CLAPHelperHost(name, vendor, url, version) {
        }
        ~RemidyCLAPHost() override;

        bool threadCheckIsMainThread() const noexcept override;

        bool threadCheckIsAudioThread() const noexcept override;

        bool implementsGui() const noexcept override { return true; }
        void guiResizeHintsChanged() noexcept override {
            Logger::global()->logWarning("guiResizeHintsChanged not implemented");
        }
        bool guiRequestShow() noexcept override;
        bool guiRequestHide() noexcept override;
        bool guiRequestResize(uint32_t width, uint32_t height) noexcept override;
        void guiClosed(bool wasDestroyed) noexcept override;

        // timer support
        bool implementsTimerSupport() const noexcept override { return true; }
        bool timerSupportRegisterTimer(uint32_t periodMs, clap_id *timerId) noexcept override;
        bool timerSupportUnregisterTimer(clap_id timerId) noexcept override;

        // log support
        bool implementsLog() const noexcept override { return true; }
        void logLog(clap_log_severity severity, const char *message) const noexcept override;

    public:
        void attachInstance(PluginInstanceCLAP* instance) noexcept;
        void detachInstance(PluginInstanceCLAP* instance) noexcept;
        PluginInstanceCLAP* attachedInstance() const noexcept { return attached_instance.load(); }
    };
}
