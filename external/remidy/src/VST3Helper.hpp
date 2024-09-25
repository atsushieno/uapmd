#pragma once

#include <codecvt>
#include <filesystem>
#include <functional>
#include <vector>

#include "../include/remidy/Common.hpp"
#include <travesty/factory.h>
#include <travesty/component.h>
#include <travesty/host.h>
#include <travesty/edit_controller.h>
#include <travesty/unit.h>
#include <travesty/view.h>

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#define kVstAudioEffectClass "Audio Module Class"

namespace remidy {

    enum V3_IO_MODES {
        V3_IO_SIMPLE,
        V3_IO_ADVANCED,
        V3_IO_OFFLINE_PROCESSING
    };

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
    struct IComponentVtable : IPluginBaseVTable {
        v3_component component;
    };
    struct IEditControllerVTable : IPluginBaseVTable {
        v3_edit_controller controller;
    };
    struct IComponentHandlerVtable : FUnknownVTable {
        v3_component_handler handler;
    };
    struct IComponentHandler2Vtable : FUnknownVTable {
        v3_component_handler2 handler;
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
        struct IComponentVtable *vtable{};
    };
    struct IEditController {
        struct IEditControllerVTable *vtable{};
    };
    struct IComponentHandler {
        struct IComponentHandlerVtable *vtable{};
    };
    struct IComponentHandler2 {
        struct IComponentHandler2Vtable *vtable{};
    };
    struct IHostApplication {
        struct IHostApplicationVTable *vtable{};
    };

    class HostApplication :
        public IComponentHandler,
        public IComponentHandler2,
        public IHostApplication
    {
        IComponentHandlerVtable handler_vtable{};
        IComponentHandler2Vtable handler_vtable2{};
        IHostApplicationVTable host_vtable{};
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

    public:
        explicit HostApplication(): IHostApplication() {
            host_vtable.unknown.query_interface = query_interface;
            host_vtable.unknown.ref = add_ref;
            host_vtable.unknown.unref = remove_ref;
            host_vtable.application.create_instance = create_instance;
            host_vtable.application.get_name = get_name;
            IHostApplication::vtable = &host_vtable;

            handler_vtable.unknown = host_vtable.unknown;
            handler_vtable.handler.begin_edit = begin_edit;
            handler_vtable.handler.end_edit = end_edit;
            handler_vtable.handler.perform_edit = perform_edit;
            handler_vtable.handler.restart_component = restart_component;
            IComponentHandler::vtable = &handler_vtable;
            handler_vtable2.unknown = host_vtable.unknown;
            IComponentHandler2::vtable = &handler_vtable2;
        }
        ~HostApplication() = default;

        v3_result queryInterface(const v3_tuid iid, void **obj);
    };
    std::filesystem::path getPluginCodeFile(std::filesystem::path& pluginPath);

    typedef IPluginFactory* (*get_plugin_factory_func)();
    typedef bool (*vst3_module_entry_func)(void*);
    typedef bool (*vst3_module_exit_func)();
    typedef bool (*vst3_bundle_entry_func)(void*);
    typedef bool (*vst3_bundle_exit_func)();
    typedef bool (*vst3_init_dll_func)();
    typedef bool (*vst3_exit_dll_func)();

    void* loadLibraryFromBundle(std::filesystem::path vst3Dir);
    int32_t initializeModule(void* library);
    void unloadLibrary(void* library);

    struct PluginClassInfo {
        std::filesystem::path bundlePath;
        std::string vendor;
        std::string url;
        std::string className;
        v3_tuid tuid{};

        PluginClassInfo(
            std::filesystem::path& bundlePath,
            std::string& vendor,
            std::string& url,
            std::string& className,
            v3_tuid tuid
        ): bundlePath(bundlePath), vendor(vendor), url(url), className(className) {
            memcpy(this->tuid, tuid, 16);
        }
    };

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );

    void scanAllAvailablePluginsFromLibrary(std::filesystem::path vst3Dir, std::vector<PluginClassInfo>& results);
}