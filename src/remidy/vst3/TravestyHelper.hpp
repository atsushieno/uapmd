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

    std::string vst3StringToStdString(v3_str_128& src);

    enum V3_IO_MODES {
        V3_IO_SIMPLE,
        V3_IO_ADVANCED,
        V3_IO_OFFLINE_PROCESSING
    };

    enum V3_CONTROLLER_NUMBERS {
        // <snip ... you can use CMIDI2_CC_XXX ... >
        V3_AFTER_TOUCH = 128,
        V3_PITCH_BEND = 129,
        V3_COUNT_CTRL_NUMBER,
        V3_CTRL_PROGRAM_CHANGE = 130,
        V3_CTRL_POLY_PRESSURE = 131,
        V3_CTRL_QUARTER_FRAME = 132,
    };

    typedef int32_t v3_unit_id;
    typedef int32_t v3_program_list_id;
    typedef double v3_note_expression_value;

    typedef uint32_t v3_note_expression_type_id;
    enum V3_NOTE_EXPRESSION_TYPE_FLAGS {
        V3_IS_BIPOLAR = 1 << 0,
        V3_IS_ONE_SHOT = 1 << 1,
        V3_IS_ABSOLUTE = 1 << 2,
        V3_ASSOCIATED_PARAMETER_ID_VALID = 1 << 3
    };

    struct v3_note_expression_value_description {
        v3_note_expression_value default_value;
        v3_note_expression_value minimum;
        v3_note_expression_value maximum;
        int32_t step_count;
    };

    struct v3_note_expression_type_info {
        v3_note_expression_type_id type_id;
        v3_str_128 title;
        v3_str_128 short_title;
        v3_str_128 units;
        int32_t unit_id;
        v3_note_expression_value_description value_desc;
        v3_param_id associated_parameter_id;
        V3_NOTE_EXPRESSION_TYPE_FLAGS flags;
    };

    struct v3_unit_handler {
#ifndef __cplusplus
        struct v3_funknown;
#endif
        v3_result (V3_API* notify_unit_selection)(void* self, v3_unit_id unitId);
        v3_result (V3_API* notify_program_list_change)(void* self, v3_program_list_id listId, int32_t programIndex);
    };
    struct v3_plug_interface_support {
#ifndef __cplusplus
        struct v3_funknown;
#endif
        v3_result (V3_API* is_plug_interface_supported)(void* self, const v3_tuid iid);
    };
    struct v3_note_expression_controller {
#ifndef __cplusplus
        struct v3_funknown;
#endif
        int32_t (V3_API* get_note_expression_count)(void* self, int32_t busIndex, int16_t channel);
        v3_result (V3_API* get_note_expression_info)(void* self, int32_t busIndex, int16_t channel, int32_t noteExpressionIndex, v3_note_expression_type_info& info);
        v3_result (V3_API* get_note_expression_string_by_value)(int32_t busIndex, int16_t channel, v3_note_expression_type_id id, v3_note_expression_value valueNormalized, v3_str_128 string);
        // should we define something for const TChar* ? For now it is altered by v3_str_128.
        v3_result (V3_API* get_note_expression_value_by_string)(int32_t busIndex, int16_t channel, v3_note_expression_type_id id, const v3_str_128 string, v3_note_expression_value &valueNormalized);
    };
    struct v3_program_list_data {
#ifndef __cplusplus
        struct v3_funknown;
#endif
        v3_result (V3_API* program_data_supported)(void* self, v3_program_list_id listId);
        v3_result (V3_API* get_program_data)(void* self, v3_program_list_id listId, int32_t programIndex, v3_bstream* data);
        v3_result (V3_API* set_program_data)(void* self, v3_program_list_id listId, int32_t programIndex, v3_bstream* data);
    };

    static constexpr const v3_tuid v3_unit_handler_iid =
        V3_ID(0x4B5147F8, 0x4654486B, 0x8DAB30BA, 0x163A3C56);
    static constexpr const v3_tuid v3_plug_interface_support_iid =
        V3_ID(0x4FB58B9E, 0x9EAA4E0F, 0xAB361C1C, 0xCCB56FEA);
    static constexpr const v3_tuid v3_note_expression_controller_iid =
        V3_ID(0xB7F8F859, 0x41234872, 0x91169581, 0x4F3721A3);
    static constexpr const v3_tuid v3_program_list_data_iid =
        V3_ID(0x8683B01F, 0x7B354F70, 0xA2651DEC, 0x353AF4FF);


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
        v3_component_handler2 handler2{nullptr};
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
    struct INoteExpressionControllerVTable : FUnknownVTable {
        v3_note_expression_controller note_expression_controller{nullptr};
    };
    struct IProgramListDataVTable : FUnknownVTable {
        v3_program_list_data program_list_data{nullptr};
    };
    struct IUnitInfoVTable : FUnknownVTable {
        v3_unit_information unit_info{nullptr};
    };
    struct IMidiMappingVTable : FUnknownVTable {
        v3_midi_mapping midi_mapping{nullptr};
    };
    struct IBStreamVTable : FUnknownVTable {
        v3_bstream stream{nullptr};
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
        v3_result get_name(v3_str_128 name) {
            return vtable->application.get_name(this, name);
        }
        v3_result create_instance(v3_tuid cid, v3_tuid iid, void** obj) {
            return vtable->application.create_instance(this, cid, iid, obj);
        }
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
    struct INoteExpressionController {
        struct INoteExpressionControllerVTable *vtable{};
    };
    // Maybe we only use IUnitInfo...?
    struct IProgramListData {
        struct IProgramListDataVTable *vtable{};
    };
    struct IUnitInfo {
        struct IUnitInfoVTable *vtable{};
    };
    struct IMidiMapping {
        struct IMidiMappingVTable *vtable{};
    };
    struct IBStream {
        struct IBStreamVTable *vtable{};
    };

    IPluginFactory* getFactoryFromLibrary(void* module);

    void forEachPlugin(std::filesystem::path vst3Dir,
        std::function<void(void* module, IPluginFactory* factory, PluginClassInfo& info)> func,
        std::function<void(void* module)> cleanup
    );
}