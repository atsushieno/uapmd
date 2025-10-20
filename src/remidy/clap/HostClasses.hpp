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

#if defined(_MSC_VER)
#define min(v1, v2) (v1 < v2 ? v1 : v2)
#else
#define min(v1, v2) std::min(v1, v2)
#endif

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
        bool guiRequestShow() noexcept override;
        bool guiRequestHide() noexcept override;
        bool guiRequestResize(uint32_t width, uint32_t height) noexcept override;
        void guiClosed(bool wasDestroyed) noexcept override;

        // timer support
        bool implementsTimerSupport() const noexcept override { return true; }
        bool timerSupportRegisterTimer(uint32_t periodMs, clap_id *timerId) noexcept override;
        bool timerSupportUnregisterTimer(clap_id timerId) noexcept override;

    public:
        void attachInstance(PluginInstanceCLAP* instance) noexcept;
        void detachInstance(PluginInstanceCLAP* instance) noexcept;
        PluginInstanceCLAP* attachedInstance() const noexcept { return attached_instance.load(); }
    };

    class RemidyCLAPEventList {
        clap_input_events_t in_events;
        clap_output_events_t out_events;

        static uint32_t remidy_clap_in_events_size(const struct clap_input_events *list) {
            return ((RemidyCLAPEventList*) list)->inEventsSize();
        }
        static const clap_event_header_t* remidy_clap_in_events_get(const struct clap_input_events *list, uint32_t index) {
            return ((RemidyCLAPEventList*) list)->inEventsGet(index);
        }
        static bool remidy_clap_out_events_try_push(const struct clap_output_events *list, const clap_event_header_t *event) {
            return ((RemidyCLAPEventList*) list)->outEventsTryPush(event);
        }
        uint32_t inEventsSize() {
            return 0;
        }
        clap_event_header_t* inEventsGet(uint32_t index) {
            return nullptr;
        }
        bool outEventsTryPush(const clap_event_header_t *event) {
            return false;
        }

    public:
        RemidyCLAPEventList(PluginInstanceCLAP* owner) {
            in_events.size = remidy_clap_in_events_size;
            in_events.get = remidy_clap_in_events_get;
            in_events.ctx = this;
            out_events.try_push = remidy_clap_out_events_try_push;
            out_events.ctx = this;
        }

        clap_input_events* inEvents() { return &in_events; }
        clap_output_events* outEvents() { return &out_events; }
    };
}
