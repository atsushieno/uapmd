#include <iostream>

#include "TravestyHelper.hpp"

namespace remidy_vst3 {

    const std::basic_string<char16_t> HostApplication::name16t{std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes("remidy")};

    v3_result HostApplication::query_interface(void *self, const v3_tuid iid, void **obj) {
        return ((HostApplication*) self)->queryInterface(iid, obj);
    }

    HostApplication::HostApplication(remidy::Logger* logger): IHostApplication(), logger(logger) {
        host_vtable.unknown.query_interface = query_interface;
        host_vtable.unknown.ref = add_ref;
        host_vtable.unknown.unref = remove_ref;
        host_vtable.application.create_instance = create_instance;
        host_vtable.application.get_name = get_name;
        vtable = &host_vtable;

        handler_vtable.unknown = host_vtable.unknown;
        handler_vtable.handler.begin_edit = begin_edit;
        handler_vtable.handler.end_edit = end_edit;
        handler_vtable.handler.perform_edit = perform_edit;
        handler_vtable.handler.restart_component = restart_component;
        handler.vtable = &handler_vtable;
        unit_handler_vtable.unknown = host_vtable.unknown;
        unit_handler_vtable.handler.notify_unit_selection = notify_unit_selection;
        unit_handler_vtable.handler.notify_program_list_change = notify_program_list_change;
        unit_handler.vtable = &unit_handler_vtable;
    }

    HostApplication::~HostApplication() {
    }

    v3_result HostApplication::queryInterface(const v3_tuid iid, void **obj) {
        if (
            !memcmp(iid, v3_host_application_iid, sizeof(v3_tuid)) ||
            !memcmp(iid, v3_funknown_iid, sizeof(v3_tuid))
        ) {
            add_ref(this);
            *obj = this;
            return V3_OK;
        }
        if (!memcmp(iid, v3_component_handler_iid, sizeof(v3_tuid))) {
            // should we addref?
            *obj = &handler;
            return V3_OK;
        }
        if (!memcmp(iid, v3_unit_handler_iid, sizeof(v3_tuid))) {
            // should we addref?
            *obj = &unit_handler;
            return V3_OK;
        }

        *obj = nullptr;
        return -1;
    }

    uint32_t HostApplication::add_ref(void *self) {
        // it seems to not be managed allocation by these refs.
        return 1; //++host->ref_counter;
    }

    uint32_t HostApplication::remove_ref(void *self) {
        // it seems to not be managed allocation by these refs.
        return 1; //--host->ref_counter;
    }

    v3_result HostApplication::create_instance(void *self, v3_tuid cid, v3_tuid iid, void **obj) {
        *obj = nullptr;
        throw std::runtime_error("HostApplication::create_instance() is not implemented");
    }

    v3_result HostApplication::get_name(void *self, v3_str_128 name) {
        name16t.copy((char16_t*) name, name16t.length());
        return V3_OK;
    }

    v3_result HostApplication::begin_edit(void *self, v3_param_id paramId) {
        std::cerr << "HostApplication::begin_edit(" << std::hex << paramId << std::dec << ") is not implemented" << std::endl;
        // FIXME: implement
        return V3_TRUE;
    }

    v3_result HostApplication::end_edit(void *self, v3_param_id paramId) {
        std::cerr << "HostApplication::end_edit(" << std::hex << paramId << std::dec << ") is not implemented" << std::endl;
        // FIXME: implement
        return V3_TRUE;
    }

    v3_result HostApplication::perform_edit(void *self, v3_param_id paramId, double value_normalised) {
        std::cerr << "HostApplication::perform_edit(" << std::hex << paramId << ", " << value_normalised << std::dec << ") is not implemented" << std::endl;
        // FIXME: implement
        return V3_TRUE;
    }

    v3_result HostApplication::restart_component(void *self, int32_t flags) {
        std::cerr << "HostApplication::restart_component(" << std::hex << flags << std::dec << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::notify_unit_selection(void *self, v3_unit_id unitId) {
        std::cerr << "HostApplication::notify_unit_selection(" << unitId << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::notify_program_list_change(void *self, v3_program_list_id listId, int32_t programIndex) {
        std::cerr << "HostApplication::notify_program_list_change(" << listId << ", " << programIndex << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }
}
