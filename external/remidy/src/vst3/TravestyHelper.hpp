#pragma once

#include <codecvt>
#include <filesystem>
#include <functional>
#include <vector>

#include "ClassModuleInfo.hpp"
#include <travesty/factory.h>
#include <travesty/component.h>
#include <travesty/host.h>
#include <travesty/edit_controller.h>
#include <travesty/unit.h>
#include <travesty/events.h>
#include <travesty/view.h>

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

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
    static constexpr const v3_tuid v3_unit_handler_iid =
        V3_ID(0x4B5147F8, 0x4654486B, 0x8DAB30BA, 0x163A3C56);

    struct FUnknownVTable {
        v3_funknown unknown;
    };
    struct IPluginFactoryVTable : public FUnknownVTable {
        v3_plugin_factory factory;
    };
    struct IPluginFactory2VTable : public IPluginFactoryVTable {
        v3_plugin_factory_2 factory_2;
    };
    struct IPluginFactory3VTable : public IPluginFactory2VTable {
        v3_plugin_factory_3 factory_3;
    };
    struct IPluginBaseVTable : FUnknownVTable {
        v3_plugin_base base;
    };
    struct IComponentVTable : IPluginBaseVTable {
        v3_component component;
    };
    struct IEditControllerVTable : IPluginBaseVTable {
        v3_edit_controller controller;
    };
    struct IComponentHandlerVTable : FUnknownVTable {
        v3_component_handler handler;
    };
    struct IComponentHandler2VTable : FUnknownVTable {
        v3_component_handler2 handler;
    };
    struct IUnitHandlerVTable : FUnknownVTable {
        v3_unit_handler handler;
    };
    struct IHostApplicationVTable : public FUnknownVTable {
        v3_host_application application;
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
    struct IEditController {
        struct IEditControllerVTable *vtable{};
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
    struct IHostApplication {
        struct IHostApplicationVTable *vtable{};
    };

    class HostApplication :
        //public IComponentHandler,
        //public IUnitHandler,
        public IHostApplication
    {
        IComponentHandlerVTable handler_vtable{};
        IUnitHandlerVTable unit_handler_vtable{};
        IHostApplicationVTable host_vtable{};
        IComponentHandler handler{nullptr};
        IUnitHandler unit_handler{nullptr};

        static const std::basic_string<char16_t> name16t;

        static v3_result query_interface(void *self, const v3_tuid iid, void **obj);
        static uint32_t add_ref(void *self);
        static uint32_t remove_ref(void *self);
        static v3_result create_instance(void *self, v3_tuid cid, v3_tuid iid, void **obj);
        static v3_result get_name(void *self, v3_str_128 name);

        static v3_result begin_edit(void *self, v3_param_id);
        static v3_result end_edit(void *self, v3_param_id);
        static v3_result perform_edit(void *self, v3_param_id, double value_normalised);
        static v3_result restart_component(void *self, int32_t flags);

        static v3_result notify_unit_selection(void *self, v3_unit_id unitId);
        static v3_result notify_program_list_change(void *self, v3_program_list_id listId, int32_t programIndex);

    public:
        explicit HostApplication(): IHostApplication() {
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
        ~HostApplication();

        v3_result queryInterface(const v3_tuid iid, void **obj);

        inline IComponentHandler* getComponentHandler() { return &handler; }
        inline IUnitHandler* getUnitHandler() { return &unit_handler; }
    };
    std::filesystem::path getPluginCodeFile(std::filesystem::path& pluginPath);

    typedef IPluginFactory* (*get_plugin_factory_func)();
    typedef bool (*vst3_module_entry_func)(void*);
    typedef bool (*vst3_module_exit_func)();
    typedef bool (*vst3_bundle_entry_func)(void*);
    typedef bool (*vst3_bundle_exit_func)();
    typedef bool (*vst3_init_dll_func)();
    typedef bool (*vst3_exit_dll_func)();

    void* loadModuleFromVst3Path(std::filesystem::path vst3Dir);
    int32_t initializeModule(void* library);
    void unloadModule(void* library);
    IPluginFactory* getFactoryFromLibrary(void* module);

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );

    void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
}