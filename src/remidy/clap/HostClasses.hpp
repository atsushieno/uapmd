#pragma once

#include <exception>
#include <clap/helpers/host.hh>
#include <clap/helpers/host.hxx>


#include "clap/events.h"

using CLAPHelperHost = clap::helpers::Host<
    clap::helpers::MisbehaviourHandler::Ignore,
    clap::helpers::CheckingLevel::Maximal
>;

#define min(v1, v2) (v1 < v2 ? v1 : v2)

namespace remidy {
    class PluginInstanceCLAP;

    class RemidyCLAPHost : public CLAPHelperHost {
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

        bool threadCheckIsMainThread() const noexcept override;

        bool threadCheckIsAudioThread() const noexcept override;
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
