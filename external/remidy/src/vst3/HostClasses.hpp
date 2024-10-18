#pragma once

#include "TravestyHelper.hpp"

namespace remidy_vst3 {

    // Host implementation
#define IMPLEMENT_FUNKNOWN_REFS(TYPE) \
    uint32_t refCount; \
    static uint32_t add_ref(void *self) { return ++((TYPE *)self)->refCount; } \
    static uint32_t remove_ref(void *self) { return --((TYPE *)self)->refCount; } \
    static v3_result query_interface(void *self, const v3_tuid iid, void **obj) { \
        return ((TYPE*) self)->queryInterface(iid, obj); \
    }
#define FILL_FUNKNOWN_VTABLE \
    refCount = 1; \
    vtable.unknown.query_interface = query_interface; \
    vtable.unknown.ref = add_ref;

    class HostAttributeList : public IAttributeList {
        IAttributeListVTable vtable;

        IMPLEMENT_FUNKNOWN_REFS(HostAttributeList)

    public:
        IAttributeListVTable* asInterface() { return &vtable; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            return V3_NO_INTERFACE;
        }

        static v3_result set_int(void *self, const char *id, int64_t value) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result get_int(void *self, const char *id, int64_t *value) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result set_float(void *self, const char *id, double value) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result get_float(void *self, const char *id, double *value) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result set_string(void *self, const char *id, const int16_t *value) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result get_string(void *self, const char *id, int16_t *value, uint32_t sizeInBytes) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }
        static v3_result set_binary(void *self, const char *id, const void *data, uint32_t sizeInBytes) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        static v3_result get_binary(void *self, const char *id, const void **data, uint32_t *sizeInBytes) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NOT_IMPLEMENTED;
        }

        explicit HostAttributeList() {
            FILL_FUNKNOWN_VTABLE
            vtable.unknown.unref = remove_ref;
            vtable.attribute_list.set_int = set_int;
            vtable.attribute_list.get_int = get_int;
            vtable.attribute_list.set_float= set_float;
            vtable.attribute_list.get_float= get_float;
            vtable.attribute_list.set_string= set_string;
            vtable.attribute_list.get_string = get_string;
            vtable.attribute_list.set_binary = set_binary;
            vtable.attribute_list.get_binary = get_binary;
        }
        ~HostAttributeList() = default;

    };

    class HostMessage {
        IMessageVTable vtable;
        v3_message *v3_impl;
        std::string id;
        HostAttributeList list{};

        IMPLEMENT_FUNKNOWN_REFS(HostMessage)

    public:
        explicit HostMessage() {
            FILL_FUNKNOWN_VTABLE

            v3_impl = &vtable.message;
            vtable.message.get_message_id = get_message_id;
            vtable.message.set_message_id = set_message_id;
            vtable.message.get_attributes = get_attributes;
        }
        ~HostMessage() = default;
        IMessageVTable* asInterface() { return &vtable; }
        auto v3() { return &v3_impl; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            if (
                !memcmp(iid, v3_message_iid, sizeof(v3_tuid)) ||
                !memcmp(iid, v3_funknown_iid, sizeof(v3_tuid))
            ) {
                add_ref(this);
                *obj = this;
                return V3_OK;
            }
            *obj = nullptr;
            return V3_NO_INTERFACE;
        }

        static const char* get_message_id(void *self) {
            auto& i = ((HostMessage*) self)->id;
            return i.empty() ? nullptr : i.c_str();
        }

        static void set_message_id(void *self, const char *id) {
            ((HostMessage*) self)->id = id;
        }

        static v3_attribute_list** get_attributes(void *self) {
            auto& list = ((HostMessage*) self)->list;
            return (v3_attribute_list**) list.asInterface();
        }
    };

    class HostEventList {
        IEventListVTable vtable{};
        v3_event_list* v3_impl;
        IMPLEMENT_FUNKNOWN_REFS(HostEventList)
        static uint32_t get_event_count(void *self);
        static v3_result get_event(void *self, int32_t index, struct v3_event* e);
        static v3_result add_event(void *self, struct v3_event *e);

    public:
        explicit HostEventList() {
            FILL_FUNKNOWN_VTABLE

            v3_impl = &vtable.event_list;
            vtable.event_list.get_event_count = get_event_count;
            vtable.event_list.get_event = get_event;
            vtable.event_list.add_event = add_event;
        }
        IEventListVTable* asInterface() { return &vtable; }
        auto v3() { return &v3_impl; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IEventList?" << std::endl;
            return V3_NO_INTERFACE;
        }
    };

    class HostParamValueQueue {
        IParamValueQueueVTable vtable;
        v3_param_value_queue *v3_impl;
        IMPLEMENT_FUNKNOWN_REFS(HostParamValueQueue)
        static v3_param_id get_param_id(void* self);
        static int32_t get_point_count(void* self);
        static v3_result get_point(void* self, int32_t idx, int32_t* sample_offset, double* value);
        static v3_result add_point(void* self, int32_t sample_offset, double value, int32_t* idx);

    public:
        explicit HostParamValueQueue() {
            FILL_FUNKNOWN_VTABLE

            v3_impl = &vtable.param_value_queue;
            vtable.param_value_queue.get_param_id = get_param_id;
            vtable.param_value_queue.get_point_count = get_point_count;
            vtable.param_value_queue.get_point = get_point;
            vtable.param_value_queue.add_point = add_point;
        }
        IParamValueQueueVTable* asInterface() { return &vtable; }
        auto v3() { return &v3_impl; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IParamValueQueue?" << std::endl;
            return V3_NO_INTERFACE;
        }
    };

    class HostParameterChanges {
        IParameterChangesVTable vtable;
        v3_param_changes *v3_impl;
        IMPLEMENT_FUNKNOWN_REFS(HostParameterChanges)
        HostParamValueQueue queue{};

        static int32_t get_param_count(void* self);
        static struct v3_param_value_queue** get_param_data(void* self, int32_t idx);
        static struct v3_param_value_queue** add_param_data(void* self, const v3_param_id* id, int32_t* idx);

    public:
        explicit HostParameterChanges() {
            FILL_FUNKNOWN_VTABLE

            v3_impl = &vtable.parameter_changes;
            vtable.parameter_changes.get_param_count = get_param_count;
            vtable.parameter_changes.get_param_data = get_param_data;
            vtable.parameter_changes.add_param_data = add_param_data;
        }
        IParameterChangesVTable* asInterface() { return &vtable; }
        auto v3() { return &v3_impl; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IParameterChanges?" << std::endl;
            return V3_NO_INTERFACE;
        }
    };

    class HostApplication :
        // we cannot simply implement them. That will mess vtables.
        //public IAttributeList,
        //public IEventHandler,
        //public IComponentHandler,
        //public IMessage,
        //public IParamValueQueue,
        //public IParameterChanges,
        //public IPlugFrame,
        //public IUnitHandler,
        //public IPlugInterfaceSupport,
        public IHostApplication
    {
        IAttributeListVTable attribute_list_vtable{};
        IEventHandlerVTable event_handler_vtable{};
        IComponentHandlerVTable handler_vtable{};
        IUnitHandlerVTable unit_handler_vtable{};
        IMessageVTable message_vtable{};
        IParamValueQueueVTable param_value_queue_table{};
        IPlugFrameVTable plug_frame_vtable{};
        IPlugInterfaceSupportVTable support_vtable{};
        IHostApplicationVTable host_vtable{};
        IAttributeList attribute_list{nullptr};
        IEventHandler event_handler{nullptr};
        IComponentHandler handler{nullptr};
        IUnitHandler unit_handler{nullptr};
        IMessage message{nullptr};
        IPlugFrame plug_frame{nullptr};
        IPlugInterfaceSupport support{nullptr};
        HostParameterChanges parameter_changes{};
        HostParamValueQueue param_value_queue{};

        remidy::Logger* logger;

        static const std::basic_string<char16_t> name16t;

        static v3_result query_interface(void *self, const v3_tuid iid, void **obj);
        static uint32_t add_ref(void *self);
        static uint32_t remove_ref(void *self);
        static v3_result create_instance(void *self, v3_tuid cid, v3_tuid iid, void **obj);
        static v3_result get_name(void *self, v3_str_128 name);

        static v3_result set_int(void *self, const char* id, int64_t value);
        static v3_result get_int(void *self, const char* id, int64_t* value);
        static v3_result set_float(void *self, const char* id, double value);
        static v3_result get_float(void *self, const char* id, double* value);
        static v3_result set_string(void *self, const char* id, const int16_t* value);
        static v3_result get_string(void *self, const char* id, int16_t* value, uint32_t sizeInBytes);
        static v3_result set_binary(void *self, const char* id, const void* data, uint32_t sizeInBytes);
        static v3_result get_binary(void *self, const char* id, const void** data, uint32_t *sizeInBytes);

        static v3_result begin_edit(void *self, v3_param_id);
        static v3_result end_edit(void *self, v3_param_id);
        static v3_result perform_edit(void *self, v3_param_id, double value_normalised);
        static v3_result restart_component(void *self, int32_t flags);

        static v3_result notify_unit_selection(void *self, v3_unit_id unitId);
        static v3_result notify_program_list_change(void *self, v3_program_list_id listId, int32_t programIndex);

        static const char* get_message_id(void *self);
        static void set_message_id(void *self, const char* id);
        static IAttributeList* get_attributes(void *self);

        static v3_result resize_view(void* self, struct v3_plugin_view**, struct v3_view_rect*);

        static v3_result is_plug_interface_supported(void* self, const v3_tuid iid);

    public:
        explicit HostApplication(remidy::Logger* logger);
        ~HostApplication();

        v3_result queryInterface(const v3_tuid iid, void **obj);

        inline IComponentHandler* getComponentHandler() { return &handler; }
        inline IUnitHandler* getUnitHandler() { return &unit_handler; }
        inline IPlugInterfaceSupport* getPlugInterfaceSupport() { return &support; }
    };
}