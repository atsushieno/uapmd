#pragma once

#include "TravestyHelper.hpp"
#include <iostream>

namespace remidy_vst3 {

    // Host implementation
#define IMPLEMENT_FUNKNOWN_REFS(TYPE) \
    uint32_t refCount{1}; \
    static uint32_t add_ref(void *self) { return ++((TYPE *)self)->refCount; } \
    static uint32_t remove_ref(void *self) { return --((TYPE *)self)->refCount; } \
    static v3_result query_interface(void *self, const v3_tuid iid, void **obj) { \
        return ((TYPE*) self)->queryInterface(iid, obj); \
    }
#define FILL_FUNKNOWN_VTABLE \
    refCount = 1; \
    vtable.unknown.query_interface = query_interface; \
    vtable.unknown.ref = add_ref; \
    vtable.unknown.unref = remove_ref;

    class HostAttributeList : public IAttributeList {
        IAttributeListVTable impl;

        IMPLEMENT_FUNKNOWN_REFS(HostAttributeList)

    public:
        auto asInterface() { return &impl; }

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
            this->vtable = &impl;
            auto& vtable = impl;
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

    class HostMessage : public IMessage {
        IMessageVTable impl;
        std::string id;
        HostAttributeList list{};

        IMPLEMENT_FUNKNOWN_REFS(HostMessage)

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

    public:
        explicit HostMessage() {
            this->vtable = &impl;
            auto& vtable = impl;
            FILL_FUNKNOWN_VTABLE

            vtable.message.get_message_id = get_message_id;
            vtable.message.set_message_id = set_message_id;
            vtable.message.get_attributes = get_attributes;
        }
        ~HostMessage() = default;
        auto asInterface() { return &impl; }

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
    };

    class HostEventList : public IEventList {
        IEventListVTable impl{};
        IMPLEMENT_FUNKNOWN_REFS(HostEventList)

        static uint32_t get_event_count(void *self) {
            return ((HostEventList*) self)->events.size();
        }
        static v3_result get_event(void *self, int32_t index, struct v3_event* e) {
            *e = ((HostEventList*) self)->events[index];
            return V3_OK;
        }
        static v3_result add_event(void *self, struct v3_event *e) {
            ((HostEventList*) self)->events.emplace_back(*e);
            return V3_OK;
        }

        std::vector<v3_event> events{};

    public:
        explicit HostEventList() {
            this->vtable = &impl;
            auto& vtable = impl;
            FILL_FUNKNOWN_VTABLE

            vtable.event_list.get_event_count = get_event_count;
            vtable.event_list.get_event = get_event;
            vtable.event_list.add_event = add_event;
        }
        auto asInterface() { return this; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IEventList?" << std::endl;
            return V3_NO_INTERFACE;
        }

        // invoked at process()
        void clear() {
            events.clear();
        }
    };

    class HostParamValueQueue : public IParamValueQueue {
        IParamValueQueueVTable impl{};
        IMPLEMENT_FUNKNOWN_REFS(HostParamValueQueue)

        struct Point {
            int32_t offset;
            double value;
        };
        const v3_param_id id;
        std::vector<Point> points{};

        static v3_param_id get_param_id(void* self) { return ((HostParamValueQueue*) self)->id; }
        static int32_t get_point_count(void* self) { return ((HostParamValueQueue*) self)->points.size(); }
        static v3_result get_point(void* self, int32_t idx, int32_t* sample_offset, double* value) {
            return ((HostParamValueQueue*) self)->getPoint(idx, sample_offset, value);
        }
        v3_result getPoint(int32_t idx, int32_t* sample_offset, double* value) {
            auto& p = points[idx];
            *sample_offset = p.offset;
            *value = p.value;
            return V3_OK;
        }
        static v3_result add_point(void* self, int32_t sample_offset, double value, int32_t* idx) {
            return ((HostParamValueQueue*) self)->addPoint(sample_offset, value, idx);
        }
        v3_result addPoint(int32_t sample_offset, double value, int32_t* idx) {
            *idx = points.size();
            Point pt{sample_offset, value};
            points.emplace_back(pt);
            return V3_OK;
        }

    public:
        explicit HostParamValueQueue(const v3_param_id* id) : id{*id} {
            this->vtable = &impl;
            auto& vtable = impl;
            FILL_FUNKNOWN_VTABLE

            vtable.param_value_queue.get_param_id = get_param_id;
            vtable.param_value_queue.get_point_count = get_point_count;
            vtable.param_value_queue.get_point = get_point;
            vtable.param_value_queue.add_point = add_point;
        }
        auto asInterface() { return this; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IParamValueQueue?" << std::endl;
            return V3_NO_INTERFACE;
        }
    };

    class HostParameterChanges : public IParameterChanges {
        IParameterChangesVTable impl{};
        IMPLEMENT_FUNKNOWN_REFS(HostParameterChanges)
        std::vector<std::unique_ptr<HostParamValueQueue>> queues{};

        static int32_t get_param_count(void* self) { return ((HostParameterChanges*) self)->queues.size(); }
        static struct v3_param_value_queue** get_param_data(void* self, int32_t idx) {
            return (v3_param_value_queue**) ((HostParameterChanges*) self)->queues[idx]->asInterface();
        }
        static struct v3_param_value_queue** add_param_data(void* self, const v3_param_id* id, int32_t* idx) {
            return ((HostParameterChanges*) self)->addParamData(id, idx);
        }
        struct v3_param_value_queue** addParamData(const v3_param_id* id, int32_t* idx) {
            for (int32_t i = 0; i < queues.size(); ++i) {
                auto iface = queues[i]->asInterface();
                if (iface->vtable->param_value_queue.get_param_id(iface) == *id) {
                    *idx = i;
                    return (v3_param_value_queue**) iface;
                }
            }
            *idx = queues.size();
            // FIXME: this should not allocate. Move it elsewhere.
            queues.emplace_back(std::make_unique<HostParamValueQueue>(id));
            return (v3_param_value_queue**) queues[*idx]->asInterface();
        }

    public:
        explicit HostParameterChanges() {
            this->vtable = &impl;
            auto& vtable = impl;
            FILL_FUNKNOWN_VTABLE

            vtable.parameter_changes.get_param_count = get_param_count;
            vtable.parameter_changes.get_param_data = get_param_data;
            vtable.parameter_changes.add_param_data = add_param_data;
        }
        IParameterChanges* asInterface() { return this; }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IParameterChanges?" << std::endl;
            return V3_NO_INTERFACE;
        }

        void startProcessing() {
            // we need to allocate memory for IParamValueQueues for all the parameters.
        }

        void stopProcessing() {
            // we need to deallocate memory for IParamValueQueues for all the parameters.
        }
    };

    class VectorStream : public IBStream {
        IBStreamVTable impl;
        IMPLEMENT_FUNKNOWN_REFS(VectorStream)
        std::vector<uint8_t>& data;
        int32_t offset{0};

        static v3_result read(void *self, void* buffer, int32_t num_bytes, int32_t* bytes_read) {
            auto v = ((VectorStream*) self);
            auto& data = v->data;
            auto size = std::min(static_cast<int32_t>(data.size() - v->offset), num_bytes);
            memcpy(buffer, data.data() + v->offset, size);
            if (bytes_read)
                *bytes_read = size;
            return V3_OK;
        }

        static v3_result write(void *self, void* buffer, int32_t num_bytes, int32_t* bytes_written) {
            auto v = ((VectorStream*) self);
            auto& data = v->data;
            data.resize(data.size() + num_bytes);
            auto size = std::min(static_cast<int32_t>(data.size() - v->offset), num_bytes);
            memcpy(data.data() + v->offset, buffer, size);
            if (bytes_written)
                *bytes_written = size;
            return V3_OK;
        }

        static v3_result seek(void *self, int64_t pos, int32_t seek_mode, int64_t* result) {
            auto v = ((VectorStream*) self);
            switch (seek_mode) {
                case v3_seek_mode::V3_SEEK_SET:
                    if (pos >= v->data.size())
                        return V3_INVALID_ARG;
                    v->offset = pos;
                    break;
                case v3_seek_mode::V3_SEEK_CUR:
                    if (v->offset + pos >= v->data.size())
                        return V3_INVALID_ARG;
                    v->offset += pos;
                    break;
                case v3_seek_mode::V3_SEEK_END:
                    if (pos > v->data.size())
                        return V3_INVALID_ARG;
                    v->offset = v->data.size() - pos;
                    break;
            }
            if (result)
                *result = v->offset;
            return V3_OK;
        }

        static v3_result tell(void * self, int64_t *pos) {
            *pos = ((VectorStream*) self)->offset;
            return V3_OK;
        }

    public:
        explicit VectorStream(std::vector<uint8_t>& data) : data(data) {
            this->vtable = &impl;
            auto& vtable = impl;
            FILL_FUNKNOWN_VTABLE
            vtable.stream.read = read;
            vtable.stream.write = write;
            vtable.stream.seek = seek;
            vtable.stream.tell = tell;
        }

        v3_result queryInterface(const v3_tuid iid, void **obj) {
            std::cerr << "WHY querying over IBStream?" << std::endl;
            return V3_NO_INTERFACE;
        }
    };

    // FIXME: we have to redesign any code that uses this class.
    // It meant to be a singleton, but apparently it is rather per-instance facade for hosting feature.
    // For example, HostParameterChanges must be per-instance, but the singleton instance is passed to
    // every IPluginFactory3.
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
        IComponentHandler2VTable handler2_vtable{};
        IUnitHandlerVTable unit_handler_vtable{};
        IMessageVTable message_vtable{};
        IParamValueQueueVTable param_value_queue_table{};
        IPlugFrameVTable plug_frame_vtable{};
        IPlugInterfaceSupportVTable support_vtable{};
        IHostApplicationVTable host_vtable{};
        IAttributeList attribute_list{nullptr};
        IEventHandler event_handler{nullptr};
        IComponentHandler handler{nullptr};
        IComponentHandler2 handler2{nullptr};
        IUnitHandler unit_handler{nullptr};
        IMessage message{nullptr};
        IPlugFrame plug_frame{nullptr};
        IPlugInterfaceSupport support{nullptr};
        HostParameterChanges parameter_changes{};
        // FIXME: there are plugins that require the following components as well:
        // - IMidiLearn (FM8)

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

        static v3_result set_dirty(void* self, v3_bool state);
        static v3_result request_open_editor(void* self, const char* name);
        static v3_result start_group_edit(void* self);
        static v3_result finish_group_edit(void* self);

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
        inline IComponentHandler2* getComponentHandler2() { return &handler2; }
        inline IUnitHandler* getUnitHandler() { return &unit_handler; }
        inline IPlugInterfaceSupport* getPlugInterfaceSupport() { return &support; }

        void startProcessing();
        void stopProcessing();

        uint32_t ref_counter{0};
    };
}