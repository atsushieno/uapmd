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
    struct IHostApplicationVTable : FUnknownVTable {
        v3_host_application application{nullptr};
    };
    struct IPlugInterfaceSupportVTable : FUnknownVTable {
        v3_plug_interface_support support{nullptr};
    };
    struct IEventListVTable : FUnknownVTable {
        v3_event_list event_list{nullptr};
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
    struct IEventList {
        struct IEventListVTable *vtable;
    };

    IPluginFactory* getFactoryFromLibrary(void* module);

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );
}