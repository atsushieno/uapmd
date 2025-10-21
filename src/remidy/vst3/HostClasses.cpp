#include <iostream>

#include "HostClasses.hpp"
#include "../utils.hpp"

namespace remidy_vst3 {

    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> u16conv;
    std::string vst3StringToStdString(v3_str_128& src) {
        return u16conv.to_bytes((char16_t *) src);
    }

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

        attribute_list.owner = this;
        attribute_list_vtable.unknown.query_interface = attribute_list_query_interface;
        attribute_list_vtable.unknown.ref = attribute_list_add_ref;
        attribute_list_vtable.unknown.unref = attribute_list_remove_ref;
        attribute_list_vtable.attribute_list.set_int = set_int;
        attribute_list_vtable.attribute_list.get_int = get_int;
        attribute_list_vtable.attribute_list.set_float = set_float;
        attribute_list_vtable.attribute_list.get_float = get_float;
        attribute_list_vtable.attribute_list.set_string = set_string;
        attribute_list_vtable.attribute_list.get_string = get_string;
        attribute_list_vtable.attribute_list.set_binary = set_binary;
        attribute_list_vtable.attribute_list.get_binary = get_binary;
        attribute_list.vtable = &attribute_list_vtable;

        event_handler.owner = this;
        event_handler_vtable.unknown.query_interface = event_handler_query_interface;
        event_handler_vtable.unknown.ref = event_handler_add_ref;
        event_handler_vtable.unknown.unref = event_handler_remove_ref;
        event_handler.vtable = &event_handler_vtable;

        handler.owner = this;
        handler_vtable.unknown.query_interface = component_handler_query_interface;
        handler_vtable.unknown.ref = component_handler_add_ref;
        handler_vtable.unknown.unref = component_handler_remove_ref;
        handler_vtable.handler.begin_edit = begin_edit;
        handler_vtable.handler.end_edit = end_edit;
        handler_vtable.handler.perform_edit = perform_edit;
        handler_vtable.handler.restart_component = restart_component;
        handler.vtable = &handler_vtable;
        handler2.owner = this;
        handler2_vtable.unknown.query_interface = component_handler2_query_interface;
        handler2_vtable.unknown.ref = component_handler2_add_ref;
        handler2_vtable.unknown.unref = component_handler2_remove_ref;
        handler2_vtable.handler2.set_dirty = set_dirty;
        handler2_vtable.handler2.request_open_editor = request_open_editor;
        handler2_vtable.handler2.start_group_edit = start_group_edit;
        handler2_vtable.handler2.finish_group_edit = finish_group_edit;
        handler2.vtable = &handler2_vtable;

        unit_handler.owner = this;
        unit_handler_vtable.unknown.query_interface = unit_handler_query_interface;
        unit_handler_vtable.unknown.ref = unit_handler_add_ref;
        unit_handler_vtable.unknown.unref = unit_handler_remove_ref;
        unit_handler_vtable.handler.notify_unit_selection = notify_unit_selection;
        unit_handler_vtable.handler.notify_program_list_change = notify_program_list_change;
        unit_handler.vtable = &unit_handler_vtable;

        message.owner = this;
        message_vtable.unknown.query_interface = message_query_interface;
        message_vtable.unknown.ref = message_add_ref;
        message_vtable.unknown.unref = message_remove_ref;
        message_vtable.message.get_message_id = get_message_id;
        message_vtable.message.set_message_id = set_message_id;
        message_vtable.message.get_attributes = (v3_attribute_list** (V3_API*)(void*)) get_attributes;
        message.vtable = &message_vtable;

        plug_frame.owner = this;
        plug_frame_vtable.unknown.query_interface = plug_frame_query_interface;
        plug_frame_vtable.unknown.ref = plug_frame_add_ref;
        plug_frame_vtable.unknown.unref = plug_frame_remove_ref;
        plug_frame_vtable.plug_frame.resize_view = resize_view;
        plug_frame.vtable = &plug_frame_vtable;

        support.owner = this;
        support_vtable.unknown.query_interface = plug_interface_support_query_interface;
        support_vtable.unknown.ref = plug_interface_support_add_ref;
        support_vtable.unknown.unref = plug_interface_support_remove_ref;
        support_vtable.support.is_plug_interface_supported = is_plug_interface_supported;
        support.vtable = &support_vtable;
    }

    HostApplication::~HostApplication() = default;

    void HostApplication::startProcessing() {
        parameter_changes.startProcessing();
    }

    void HostApplication::stopProcessing() {
        parameter_changes.stopProcessing();
    }

#define QUERY_HOST_INTERFACE(target, member) \
    if (v3_tuid_match(iid,target)) { \
        if ((member).vtable && (member).vtable->unknown.ref) \
            (member).vtable->unknown.ref(&(member)); \
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
        // This should be actually queries from IComponentHandler, but in this implementation
        // IComponentHandler is HostApplication itself, so this should be fine.
        QUERY_HOST_INTERFACE(v3_component_handler2_iid, handler2)
        QUERY_HOST_INTERFACE(v3_message_iid, message)
        //QUERY_HOST_INTERFACE(v3_param_value_queue_iid, param_value_queue)
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
        return ++((HostApplication*)self)->ref_counter;
    }

    uint32_t HostApplication::remove_ref(void *self) {
        // it seems to not be managed allocation by these refs.
        return --((HostApplication*)self)->ref_counter;
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

    // IAttributeList
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

    v3_result HostApplication::attribute_list_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<AttributeListImpl*>(self);
        if (v3_tuid_match(iid, v3_attribute_list_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            attribute_list_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::attribute_list_add_ref(void *self) {
        auto impl = static_cast<AttributeListImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::attribute_list_remove_ref(void *self) {
        auto impl = static_cast<AttributeListImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
    }

    v3_result HostApplication::event_handler_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<EventHandlerImpl*>(self);
        if (v3_tuid_match(iid, v3_event_handler_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            event_handler_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::event_handler_add_ref(void *self) {
        auto impl = static_cast<EventHandlerImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::event_handler_remove_ref(void *self) {
        auto impl = static_cast<EventHandlerImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
    }

    // IComponentHandler
    v3_result HostApplication::begin_edit(void *self, v3_param_id paramId) {
        // Called when user starts editing a parameter (mouse down, touch start, etc.)
        // Host should begin automation recording if applicable
        // std::cerr << "HostApplication::begin_edit(param=0x" << std::hex << paramId << std::dec << ")" << std::endl;
        return V3_OK;
    }

    v3_result HostApplication::end_edit(void *self, v3_param_id paramId) {
        // Called when user finishes editing a parameter (mouse up, touch end, etc.)
        // Host should end automation recording if applicable
        // std::cerr << "HostApplication::end_edit(param=0x" << std::hex << paramId << std::dec << ")" << std::endl;
        return V3_OK;
    }

    v3_result HostApplication::perform_edit(void *self, v3_param_id paramId, double value_normalised) {
        auto* handler = static_cast<ComponentHandlerImpl*>(self);
        auto* owner = handler->owner;

        // Find and call the registered parameter edit handler
        // We iterate through all handlers since we don't know which controller is calling
        // In practice, there's typically only one or a few active plugins
        bool handled = false;
        for (auto& [controller, callback] : owner->parameter_edit_handlers) {
            if (callback) {
                callback(paramId, value_normalised);
                handled = true;
                // Note: We call ALL registered handlers since we can't determine the caller
                // This shouldn't be a problem in practice as each plugin only affects its own state
            }
        }

        if (!handled) {
            std::cerr << "HostApplication::perform_edit(" << std::hex << paramId << ", " << value_normalised << std::dec << ") - no handler registered" << std::endl;
        }

        return V3_OK;
    }

    v3_result HostApplication::restart_component(void *self, int32_t flags) {
        // Plugins call this to notify the host about changes
        // Common flags:
        // kReloadComponent = 1 << 0,          // The whole component should be reloaded
        // kIoChanged = 1 << 1,                 // Input/Output bus configuration changed
        // kParamValuesChanged = 1 << 2,        // Multiple parameter values changed
        // kLatencyChanged = 1 << 3,            // Latency changed
        // kParamTitlesChanged = 1 << 4,        // Parameter titles changed
        // kMidiCCAssignmentChanged = 1 << 5,   // MIDI CC assignment changed
        // kNoteExpressionChanged = 1 << 6,     // Note expression changed
        // kIoTitlesChanged = 1 << 7,           // I/O titles changed
        // kPrefetchableSupportChanged = 1 << 8,// Prefetchable support changed
        // kRoutingInfoChanged = 1 << 9         // Routing info changed

        std::cerr << "HostApplication::restart_component(flags=0x" << std::hex << flags << std::dec << ")" << std::endl;

        // For now, acknowledge the restart request
        // A full implementation would handle each flag appropriately
        return V3_OK;
    }

    v3_result HostApplication::component_handler_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto owner = static_cast<ComponentHandlerImpl*>(self)->owner;
        return query_interface(owner, iid, obj);
    }

    uint32_t HostApplication::component_handler_add_ref(void *self) {
        auto owner = static_cast<ComponentHandlerImpl*>(self)->owner;
        return add_ref(owner);
    }

    uint32_t HostApplication::component_handler_remove_ref(void *self) {
        auto owner = static_cast<ComponentHandlerImpl*>(self)->owner;
        return remove_ref(owner);
    }

    v3_result HostApplication::component_handler2_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto owner = static_cast<ComponentHandler2Impl*>(self)->owner;
        return query_interface(owner, iid, obj);
    }

    uint32_t HostApplication::component_handler2_add_ref(void *self) {
        auto owner = static_cast<ComponentHandler2Impl*>(self)->owner;
        return add_ref(owner);
    }

    uint32_t HostApplication::component_handler2_remove_ref(void *self) {
        auto owner = static_cast<ComponentHandler2Impl*>(self)->owner;
        return remove_ref(owner);
    }

    v3_result HostApplication::unit_handler_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<UnitHandlerImpl*>(self);
        if (v3_tuid_match(iid, v3_unit_handler_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            unit_handler_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::unit_handler_add_ref(void *self) {
        auto impl = static_cast<UnitHandlerImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::unit_handler_remove_ref(void *self) {
        auto impl = static_cast<UnitHandlerImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
    }

    v3_result HostApplication::message_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<MessageImpl*>(self);
        if (v3_tuid_match(iid, v3_message_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            message_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::message_add_ref(void *self) {
        auto impl = static_cast<MessageImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::message_remove_ref(void *self) {
        auto impl = static_cast<MessageImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
    }

    // IComponentHandler2
    v3_result HostApplication::set_dirty(void* self, v3_bool state) {
        // Plugin notifies host that its state is dirty (has unsaved changes)
        // std::cerr << "HostApplication::set_dirty(" << (state ? "true" : "false") << ")" << std::endl;
        // Acknowledge but don't need to do anything for now
        return V3_OK;
    }

    v3_result HostApplication::request_open_editor(void* self, const char* name) {
        // Plugin requests host to open a specific editor (e.g., "Generic Editor")
        std::cerr << "HostApplication::request_open_editor(" << (name ? name : "null") << ")" << std::endl;
        // We don't support this yet, but acknowledge the request
        return V3_OK;
    }

    v3_result HostApplication::start_group_edit(void* self) {
        // Begin a group of parameter changes (for undo/redo)
        // std::cerr << "HostApplication::start_group_edit()" << std::endl;
        return V3_OK;
    }

    v3_result HostApplication::finish_group_edit(void* self) {
        // End a group of parameter changes (for undo/redo)
        // std::cerr << "HostApplication::finish_group_edit()" << std::endl;
        return V3_OK;
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
        auto impl = static_cast<MessageImpl*>(self);
        std::cerr << "HostApplication::get_message_id() is not implemented" << std::endl;
        return nullptr;
    }

    void HostApplication::set_message_id(void *self, const char *id) {
        // FIXME: implement
        auto impl = static_cast<MessageImpl*>(self);
        (void) impl;
        std::cerr << "HostApplication::set_message_id() is not implemented" << std::endl;
    }

    IAttributeList * HostApplication::get_attributes(void *self) {
        auto impl = static_cast<MessageImpl*>(self);
        return impl->owner ? static_cast<IAttributeList*>(&impl->owner->attribute_list) : nullptr;
    }

    v3_result HostApplication::plug_frame_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<PlugFrameImpl*>(self);
        if (v3_tuid_match(iid, v3_plugin_frame_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            plug_frame_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::plug_frame_add_ref(void *self) {
        auto impl = static_cast<PlugFrameImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::plug_frame_remove_ref(void *self) {
        auto impl = static_cast<PlugFrameImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
    }

    v3_result HostApplication::resize_view(void *self, struct v3_plugin_view **view, struct v3_view_rect *rect) {
        auto impl = static_cast<PlugFrameImpl*>(self);
        if (!impl || !impl->owner)
            return V3_NOT_IMPLEMENTED;

        if (!view || !*view) {
            std::cerr << "HostApplication::resize_view() called with null view" << std::endl;
            return V3_INVALID_ARG;
        }

        auto& handlers = impl->owner->resize_request_handlers;
        auto it = handlers.find(view);
        if (it == handlers.end()) {
            std::cerr << "HostApplication::resize_view() handler is not set for view " << view << std::endl;
            return V3_NOT_IMPLEMENTED;
        }

        if (!rect)
            return V3_INVALID_ARG;

        uint32_t width = rect->right - rect->left;
        uint32_t height = rect->bottom - rect->top;

        bool success = it->second(width, height);
        return success ? V3_OK : V3_NOT_IMPLEMENTED;
    }

    v3_result HostApplication::plug_interface_support_query_interface(void *self, const v3_tuid iid, void **obj) {
        auto impl = static_cast<PlugInterfaceSupportImpl*>(self);
        if (v3_tuid_match(iid, v3_plug_interface_support_iid) || v3_tuid_match(iid, v3_funknown_iid)) {
            plug_interface_support_add_ref(self);
            *obj = self;
            return V3_OK;
        }
        if (impl->owner)
            return impl->owner->queryInterface(iid, obj);
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    uint32_t HostApplication::plug_interface_support_add_ref(void *self) {
        auto impl = static_cast<PlugInterfaceSupportImpl*>(self);
        return impl->owner ? add_ref(impl->owner) : 0;
    }

    uint32_t HostApplication::plug_interface_support_remove_ref(void *self) {
        auto impl = static_cast<PlugInterfaceSupportImpl*>(self);
        return impl->owner ? remove_ref(impl->owner) : 0;
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
}
