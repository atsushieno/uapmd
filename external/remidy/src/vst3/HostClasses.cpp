#include <iostream>

#include "HostClasses.hpp"
#include "../utils.hpp"

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

        attribute_list_vtable.unknown = host_vtable.unknown;
        attribute_list_vtable.attribute_list.set_int = set_int;
        attribute_list_vtable.attribute_list.get_int = get_int;
        attribute_list_vtable.attribute_list.set_float = set_float;
        attribute_list_vtable.attribute_list.get_float = get_float;
        attribute_list_vtable.attribute_list.set_string = set_string;
        attribute_list_vtable.attribute_list.get_string = get_string;
        attribute_list_vtable.attribute_list.set_binary = set_binary;
        attribute_list_vtable.attribute_list.get_binary = get_binary;
        attribute_list.vtable = &attribute_list_vtable;

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

        message_vtable.unknown = host_vtable.unknown;
        message_vtable.message.get_message_id = get_message_id;
        message_vtable.message.set_message_id = set_message_id;
        message_vtable.message.get_attributes = (v3_attribute_list** (V3_API*)(void*)) get_attributes;
        message.vtable = &message_vtable;

        plug_frame_vtable.unknown = host_vtable.unknown;
        plug_frame_vtable.plug_frame.resize_view = resize_view;
        plug_frame.vtable = &plug_frame_vtable;

        support_vtable.unknown = host_vtable.unknown;
        support_vtable.support.is_plug_interface_supported = is_plug_interface_supported;
        support.vtable = &support_vtable;
    }

    HostApplication::~HostApplication() = default;

#define QUERY_HOST_INTERFACE(target, member) \
    if (v3_tuid_match(iid,v3_param_changes_iid)) { \
        add_ref(&(member)); \
        *obj = &(member); \
        return V3_OK; \
    }

    v3_result HostApplication::queryInterface(const v3_tuid iid, void **obj) {
        if (
            v3_tuid_match(iid,v3_funknown_iid) ||
            v3_tuid_match(iid,v3_host_application_iid)
        ) {
            add_ref(this);
            *obj = this;
            return V3_OK;
        }
        QUERY_HOST_INTERFACE(v3_component_handler_iid, handler)
        QUERY_HOST_INTERFACE(v3_message_iid, message)
        QUERY_HOST_INTERFACE(v3_param_value_queue_iid, param_value_queue)
        QUERY_HOST_INTERFACE(v3_plugin_frame_iid, plug_frame)
        QUERY_HOST_INTERFACE(v3_unit_handler_iid, unit_handler)
        QUERY_HOST_INTERFACE(v3_plug_interface_support_iid, support)

        if (v3_tuid_match(iid,v3_param_changes_iid)) {
            auto iface = parameter_changes.asInterface();
            iface->vtable->unknown.ref(iface);
            *obj = &parameter_changes;
            return V3_OK;
        }

        *obj = nullptr;
        return V3_NO_INTERFACE;
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
        if (v3_tuid_match(cid, v3_attribute_list_iid))
            *obj = new HostAttributeList();
        else if (v3_tuid_match(cid, v3_message_iid))
            *obj = new HostMessage();
        else
            return V3_NO_INTERFACE;
        return V3_OK;
    }

    v3_result HostApplication::get_name(void *self, v3_str_128 name) {
        name16t.copy((char16_t*) name, name16t.length());
        return V3_OK;
    }

    v3_result HostApplication::set_int(void *self, const char *id, int64_t value) {
        // FIXME: implement
        std::cerr << "HostApplication::set_int(" << id << "," << value << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::get_int(void *self, const char *id, int64_t *value) {
        // FIXME: implement
        std::cerr << "HostApplication::get_int(" << id << ", ..) is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::set_float(void *self, const char *id, double value) {
        // FIXME: implement
        std::cerr << "HostApplication::set_float(" << id << "," << value << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::get_float(void *self, const char *id, double *value) {
        // FIXME: implement
        std::cerr << "HostApplication::get_float(" << id << ", ..) is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::set_string(void *self, const char *id, const int16_t *value) {
        // FIXME: implement
        std::cerr << "HostApplication::set_string(" << id << "," << value << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::get_string(void *self, const char *id, int16_t *value, uint32_t sizeInBytes) {
        // FIXME: implement
        std::cerr << "HostApplication::get_string(" << id << ", ..) is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::set_binary(void *self, const char *id, const void *data, uint32_t sizeInBytes) {
        // FIXME: implement
        std::cerr << "HostApplication::set_binary(" << id << ", ..) is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::get_binary(void *self, const char *id, const void **data, uint32_t *sizeInBytes) {
        // FIXME: implement
        std::cerr << "HostApplication::get_binary(" << id << ", ..) is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
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
        // FIXME: implement
        std::cerr << "HostApplication::restart_component(" << std::hex << flags << std::dec << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::notify_unit_selection(void *self, v3_unit_id unitId) {
        // FIXME: implement
        std::cerr << "HostApplication::notify_unit_selection(" << unitId << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::notify_program_list_change(void *self, v3_program_list_id listId, int32_t programIndex) {
        // FIXME: implement
        std::cerr << "HostApplication::notify_program_list_change(" << listId << ", " << programIndex << ") is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    const char * HostApplication::get_message_id(void *self) {
        // FIXME: implement
        std::cerr << "HostApplication::get_message_id() is not implemented" << std::endl;
        return nullptr;
    }

    void HostApplication::set_message_id(void *self, const char *id) {
        // FIXME: implement
        std::cerr << "HostApplication::set_message_id() is not implemented" << std::endl;
    }

    IAttributeList * HostApplication::get_attributes(void *self) {
        return &((HostApplication*) self)->attribute_list;
    }

    v3_result HostApplication::resize_view(void *self, struct v3_plugin_view **, struct v3_view_rect *) {
        // FIXME: implement
        std::cerr << "HostApplication::resize_view() is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::is_plug_interface_supported(void* self, const v3_tuid iid) {
        return
            v3_tuid_match(iid, v3_attribute_list_iid) ||
            v3_tuid_match(iid, v3_event_handler_iid) ||
            v3_tuid_match(iid, v3_component_handler_iid) ||
            v3_tuid_match(iid, v3_unit_handler_iid) ||
            v3_tuid_match(iid, v3_message_iid) ||
            v3_tuid_match(iid, v3_param_value_queue_iid) ||
            v3_tuid_match(iid, v3_param_changes_iid) ||
            v3_tuid_match(iid, v3_plugin_frame_iid) ||
            v3_tuid_match(iid, v3_plug_interface_support_iid) ||
            v3_tuid_match(iid, v3_host_application_iid) ?
            V3_TRUE : V3_FALSE;
    }

    uint32_t HostEventList::get_event_count(void *self) {
        // FIXME: implement
        std::cerr << "HostEventList::get_event_count() is not implemented" << std::endl;
        return 0;
    }

    v3_result HostEventList::get_event(void *self, int32_t index, v3_event *e) {
        // FIXME: implement
        std::cerr << "HostEventList::get_event() is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostEventList::add_event(void *self, v3_event *e) {
        // FIXME: implement
        std::cerr << "HostEventList::add_event() is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_param_id HostParamValueQueue::get_param_id(void *self) {
        // FIXME: implement
        std::cerr << "HostParamValueQueue::get_param_id() is not implemented" << std::endl;
        return 0;
    }

    int32_t HostParamValueQueue::get_point_count(void *self) {
        // FIXME: implement
        std::cerr << "HostParamValueQueue::get_point_count() is not implemented" << std::endl;
        return 0;
    }

    v3_result HostParamValueQueue::get_point(void *self, int32_t idx, int32_t *sample_offset, double *value) {
        // FIXME: implement
        std::cerr << "HostParamValueQueue::get_point() is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    v3_result HostParamValueQueue::add_point(void *self, int32_t sample_offset, double value, int32_t *idx) {
        // FIXME: implement
        std::cerr << "HostParamValueQueue::add_point() is not implemented" << std::endl;
        return V3_NOT_IMPLEMENTED;
    }

    int32_t HostParameterChanges::get_param_count(void *self) {
        // FIXME: implement
        std::cerr << "HostParameterChanges::get_param_count() is not implemented" << std::endl;
        return 0;
    }

    struct v3_param_value_queue** HostParameterChanges::get_param_data(void *self, int32_t idx) {
        // FIXME: implement
        std::cerr << "HostParameterChanges::get_param_data() is not implemented" << std::endl;
        return (v3_param_value_queue**) ((HostParameterChanges*) self)->queue.asInterface();
    }

    struct v3_param_value_queue** HostParameterChanges::add_param_data(void *self, const v3_param_id *id, int32_t *idx) {
        // FIXME: implement
        std::cerr << "HostParameterChanges::add_param_data() is not implemented" << std::endl;
        return (v3_param_value_queue**) ((HostParameterChanges*) self)->queue.asInterface();
    }

}
