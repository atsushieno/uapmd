#pragma once

#include <codecvt>
#include <filesystem>
#include <functional>
#include <vector>

#include <travesty/factory.h>
#include <travesty/component.h>
#include <travesty/host.h>
#include <travesty/audio_processor.h>
#include <travesty/edit_controller.h>
#include <travesty/unit.h>
#include <travesty/events.h>
#include <travesty/view.h>

#include <priv/common.hpp>// for Logger
#include "ClassModuleInfo.hpp"

namespace remidy_vst3 {

    enum V3_IO_MODES {
        V3_IO_SIMPLE,
        V3_IO_ADVANCED,
        V3_IO_OFFLINE_PROCESSING
    };

    typedef int32_t v3_unit_id;
    typedef int32_t v3_program_list_id;
    struct v3_unit_handler {
#ifndef __cplusplus
        struct v3_funknown;
#endif
        v3_result (V3_API* notify_unit_selection)(void* self, v3_unit_id unitId);
        v3_result (V3_API* notify_program_list_change)(void* self, v3_program_list_id listId, int32_t programIndex);
    };
    struct v3_plug_interface_support {
        v3_result (V3_API* is_plug_interface_supported)(void* self, const v3_tuid iid);
    };

    static constexpr const v3_tuid v3_unit_handler_iid =
        V3_ID(0x4B5147F8, 0x4654486B, 0x8DAB30BA, 0x163A3C56);
    static constexpr const v3_tuid v3_plug_interface_support_iid =
        V3_ID(0x4FB58B9E, 0x9EAA4E0F, 0xAB361C1C, 0xCCB56FEA);

    struct FUnknownVTable {
        v3_funknown unknown{nullptr};
    };
    struct IPluginFactoryVTable : public FUnknownVTable {
        v3_plugin_factory factory{nullptr};
    };
    struct IPluginFactory2VTable : public IPluginFactoryVTable {
        v3_plugin_factory_2 factory_2{nullptr};
    };
    struct IPluginFactory3VTable : public IPluginFactory2VTable {
        v3_plugin_factory_3 factory_3{nullptr};
    };
    struct IPluginBaseVTable : FUnknownVTable {
        v3_plugin_base base{nullptr};
    };
    struct IComponentVTable : IPluginBaseVTable {
        v3_component component{nullptr};
    };
    struct IAudioProcessorVTable : FUnknownVTable {
        v3_audio_processor processor{nullptr};
    };
    struct IEditControllerVTable : IPluginBaseVTable {
        v3_edit_controller controller{nullptr};
    };
    struct IAttributeListVTable : FUnknownVTable {
        v3_attribute_list attribute_list{nullptr};
    };
    struct IEventHandlerVTable : FUnknownVTable {
        v3_event_handler event_handler{nullptr};
    };
    struct IComponentHandlerVTable : FUnknownVTable {
        v3_component_handler handler{nullptr};
    };
    struct IComponentHandler2VTable : FUnknownVTable {
        v3_component_handler2 handler{nullptr};
    };
    struct IUnitHandlerVTable : FUnknownVTable {
        v3_unit_handler handler{nullptr};
    };
    struct IMessageVTable : FUnknownVTable {
        v3_message message{nullptr};
    };
    struct IParamValueQueueVTable : FUnknownVTable {
        v3_param_value_queue param_value_queue{nullptr};
    };
    struct IParameterChangesVTable : FUnknownVTable {
        v3_param_changes parameter_changes{nullptr};
    };
    struct IPlugFrameVTable : FUnknownVTable {
        v3_plugin_frame plug_frame{nullptr};
    };
    struct IConnectionPointVTable : FUnknownVTable {
        v3_connection_point connection_point{nullptr};
    };
    struct IHostApplicationVTable : public FUnknownVTable {
        v3_host_application application{nullptr};
    };
    struct IPlugInterfaceSupportVTable : public FUnknownVTable {
        v3_plug_interface_support support{nullptr};
    };

    struct FUnknown {
        FUnknownVTable *vtable{};
    };
    struct IPluginFactory {
        struct IPluginFactoryVTable *vtable{};
    };
    struct IPluginFactory2 {
        struct IPluginFactory2VTable *vtable{};
    };
    struct IPluginFactory3 {
        struct IPluginFactory3VTable *vtable{};
    };
    struct IComponent {
        struct IComponentVTable *vtable{};
    };
    struct IAudioProcessor {
        struct IAudioProcessorVTable *vtable{};
    };
    struct IEditController {
        struct IEditControllerVTable *vtable{};
    };
    struct IAttributeList {
        struct IAttributeListVTable *vtable{};
    };
    struct IEventHandler {
        struct IEventHandlerVTable *vtable{};
    };
    struct IComponentHandler {
        struct IComponentHandlerVTable *vtable{};
    };
    struct IComponentHandler2 {
        struct IComponentHandler2VTable *vtable{};
    };
    struct IUnitHandler {
        struct IUnitHandlerVTable *vtable{};
    };
    struct IMessage {
        struct IMessageVTable *vtable{};
    };
    struct IParamValueQueue {
        struct IParamValueQueueVTable *vtable{};
    };
    struct IParameterChanges {
        struct IParameterChangesVTable *vtable{};
    };
    struct IPlugFrame {
        struct IPlugFrameVTable *vtable{};
    };
    struct IHostApplication {
        struct IHostApplicationVTable *vtable{};
    };
    struct IConnectionPoint {
        struct IConnectionPointVTable *vtable{};
    };
    struct IPlugInterfaceSupport {
        struct IPlugInterfaceSupportVTable *vtable{};
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
        IParameterChangesVTable parameter_changes_vtable{};
        IPlugFrameVTable plug_frame_vtable{};
        IPlugInterfaceSupportVTable support_vtable{};
        IHostApplicationVTable host_vtable{};
        IAttributeList attribute_list{nullptr};
        IEventHandler event_handler{nullptr};
        IComponentHandler handler{nullptr};
        IUnitHandler unit_handler{nullptr};
        IMessage message{nullptr};
        IParamValueQueue param_value_queue{nullptr};
        IParameterChanges parameter_changes{nullptr};
        IPlugFrame plug_frame{nullptr};
        IPlugInterfaceSupport support{nullptr};

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

        static int32_t get_event_count(void *self);
        static v3_result get_event(void *self, int32_t index, v3_event &e);
        static v3_result add_event(void *self, v3_event &e);

        static v3_result begin_edit(void *self, v3_param_id);
        static v3_result end_edit(void *self, v3_param_id);
        static v3_result perform_edit(void *self, v3_param_id, double value_normalised);
        static v3_result restart_component(void *self, int32_t flags);

        static v3_result notify_unit_selection(void *self, v3_unit_id unitId);
        static v3_result notify_program_list_change(void *self, v3_program_list_id listId, int32_t programIndex);

        static const char* get_message_id(void *self);
        static void set_message_id(void *self, const char* id);
        static IAttributeList* get_attributes(void *self);

        static v3_param_id get_param_id(void* self);
        static int32_t get_point_count(void* self);
        static v3_result get_point(void* self, int32_t idx, int32_t* sample_offset, double* value);
        static v3_result add_point(void* self, int32_t sample_offset, double value, int32_t* idx);

        static int32_t get_param_count(void* self);
        static struct v3_param_value_queue** get_param_data(void* self, int32_t idx);
        static struct v3_param_value_queue** add_param_data(void* self, const v3_param_id* id, int32_t* idx);

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

    class HostAttributeList : public IAttributeList {
        IAttributeList list;
        uint32_t refCount;

    public:
        IAttributeListVTable vtable;

        static v3_result query_interface(void *self, const v3_tuid iid, void **obj) {
            // FIXME: remove when we move this to impl. code.
            printf("WHY HERE?");
            return V3_NO_INTERFACE;
        }
        static uint32_t add_ref(void *self) {
            return ++((HostAttributeList *)self)->refCount;
        }
        static uint32_t remove_ref(void *self) {
            return --((HostAttributeList *)self)->refCount;
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

        explicit HostAttributeList() : refCount(1) {
            vtable.unknown.query_interface = query_interface;
            vtable.unknown.ref = add_ref;
            vtable.unknown.unref = remove_ref;
            vtable.attribute_list.set_int = set_int;
            vtable.attribute_list.get_int = get_int;
            vtable.attribute_list.set_float= set_float;
            vtable.attribute_list.get_float= get_float;
            vtable.attribute_list.set_string= set_string;
            vtable.attribute_list.get_string = get_string;
            vtable.attribute_list.set_binary = set_binary;
            vtable.attribute_list.get_binary = get_binary;
            list.vtable = &vtable;
        }
        ~HostAttributeList() = default;

    };

    class HostMessage {
        IMessageVTable vtable;
        IMessage msg;
        uint32_t refCount;
        std::string id;
        HostAttributeList list;

    public:
        static v3_result query_interface(void *self, const v3_tuid iid, void **obj) {
            return ((HostMessage *)self)->queryInterface(iid, obj);
        }
        static uint32_t add_ref(void *self) {
            return ++((HostMessage *)self)->refCount;
        }
        static uint32_t remove_ref(void *self) {
            return --((HostMessage *)self)->refCount;
        }

        explicit HostMessage() : refCount(1), list(list) {
            vtable.unknown.query_interface = query_interface;
            vtable.unknown.ref = add_ref;
            vtable.unknown.unref = remove_ref;
            vtable.message.get_message_id = get_message_id;
            vtable.message.set_message_id = set_message_id;
            vtable.message.get_attributes = get_attributes;
            msg.vtable = &vtable;
        }
        ~HostMessage() = default;

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
            return (v3_attribute_list**) &list.vtable;
        }
    };

    IPluginFactory* getFactoryFromLibrary(void* module);

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );
}