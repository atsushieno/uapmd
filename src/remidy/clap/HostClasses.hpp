#pragma once

#include <clap/host.h>

#include "clap/events.h"

namespace remidy {
    class PluginInstanceCLAP;

    class RemidyCLAPHost {
        clap_host_t host{};

        static const void* remidy_clap_get_extension(const struct clap_host *host, const char *extension_id) {
            return ((RemidyCLAPHost*) host)->getExtension(extension_id);
        }
        static void remidy_clap_request_callback(const struct clap_host *host) {
            ((RemidyCLAPHost*) host)->requestCallback();
        }
        static void remidy_clap_request_process(const struct clap_host *host) {
            ((RemidyCLAPHost*) host)->requestProcess();
        }
        static void remidy_clap_request_restart(const struct clap_host *host) {
            ((RemidyCLAPHost*) host)->requestRestart();
        }

        const void* getExtension(const char *extensionId) {
            // FIXME: implement
            return nullptr;
        }
        void requestCallback() {
            // FIXME: implement
        }
        void requestProcess() {
            // FIXME: implement
        }
        void requestRestart() {
            // FIXME: implement
        }

    public:
        RemidyCLAPHost(const char* name = "remidy", const char* url = "", const char* vendor = "", const char* version = "") {
            host.name = name;
            host.url = url;
            host.vendor = vendor;
            host.version = version;

            host.clap_version = CLAP_VERSION;

            host.get_extension = remidy_clap_get_extension;
            host.host_data = this;
            host.request_callback = remidy_clap_request_callback;
            host.request_process = remidy_clap_request_process;
            host.request_restart = remidy_clap_request_restart;
        }

        clap_host_t* getHost() { return &host; }
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
