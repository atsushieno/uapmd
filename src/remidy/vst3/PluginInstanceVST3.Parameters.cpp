
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::PluginInstanceVST3::ParameterSupport::ParameterSupport(PluginInstanceVST3* owner) : owner(owner) {
    auto controller = owner->controller;
    auto count = controller->vtable->controller.get_parameter_count(controller);

    parameter_ids = (v3_param_id*) calloc(sizeof(v3_param_id), count);

    for (auto i = 0; i < count; i++) {
        v3_param_info info{};
        controller->vtable->controller.get_parameter_info(controller, i, &info);
        std::string idString = std::format("{}", info.param_id);
        std::string name = vst3StringToStdString(info.title);
        std::string path{""};

        std::vector<ParameterEnumeration> enums{};
        if (info.step_count > 0)
            for (int32_t e = 0; e < info.step_count; e++) {
                v3_str_128 name{};
                auto normalized = controller->vtable->controller.plain_parameter_to_normalised(controller, info.param_id, e);
                controller->vtable->controller.get_parameter_string_for_value(controller, info.param_id, normalized, name);
                auto nameString = vst3StringToStdString(name);
                ParameterEnumeration p{nameString, normalized};
                enums.emplace_back(p);
            }

        auto p = new PluginParameter(i, idString, name, path,
                                     info.flags & V3_PARAM_IS_LIST ? info.default_normalised_value * info.step_count : info.default_normalised_value,
                                     0,
                                     info.flags & V3_PARAM_IS_LIST ? info.step_count : 1,
                                     true,
                                     info.flags & V3_PARAM_IS_HIDDEN,
                                     info.flags & V3_PARAM_IS_LIST,
                                     enums);

        parameter_ids[i] = info.param_id;
        parameter_defs.emplace_back(p);
    }
}

remidy::PluginInstanceVST3::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_defs)
        delete p;
    for (auto pair : per_note_controller_defs)
        for (auto p : pair.second)
            delete p;
    free(parameter_ids);
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceVST3::ParameterSupport::perNoteControllers(
    PerNoteControllerContextTypes types,
    PerNoteControllerContext context
) {
    for (auto& p : per_note_controller_defs)
        if (p.first.group == context.group && p.first.channel == context.channel)
            return p.second;
    std::vector<PluginParameter*> defs{};
    // populate the PNC list only if INoteExpressionController is implemented...
    auto nec = owner->note_expression_controller;
    if (nec) {
        auto count = nec->vtable->note_expression_controller.get_note_expression_count(nec, context.group, context.channel);
        for (auto i = 0; i < count; i++) {
            v3_note_expression_type_info info{};
            if (nec->vtable->note_expression_controller.get_note_expression_info(nec, context.group, context.channel, i, info) == V3_OK) {
                auto name = vst3StringToStdString(info.short_title);
                static std::string empty{};
                auto parameter = new PluginParameter(i, name, name, empty,
                    info.value_desc.default_value,
                    info.value_desc.minimum,
                    info.value_desc.maximum,
                    true, false, false);
                defs.emplace_back(parameter);
            }
        }
    }
    const auto ctx = PerNoteControllerContext{0, context.channel, context.group, 0};
    per_note_controller_defs.emplace_back(ctx, defs);
    return perNoteControllers(types, ctx);
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // use IParameterChanges.
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    auto pvc = owner->processDataInputParameterChanges.asInterface();
    const v3_param_id id = parameter_ids[index];
    int32_t i = 0;
    IParamValueQueue* q{nullptr};
    for (int32_t n = pvc->vtable->parameter_changes.get_param_count(pvc); i < n; i++) {
        q = reinterpret_cast<IParamValueQueue *>(pvc->vtable->parameter_changes.get_param_data(pvc, i));
        if (q->vtable->param_value_queue.get_param_id(q) == id)
            break;
    }
    if (!q)
        // It is an RT-safe operation, right?
        q = reinterpret_cast<IParamValueQueue *>(pvc->vtable->parameter_changes.add_param_data(pvc, &id, &i));
    if (q && q->vtable->param_value_queue.add_point(q, sampleOffset, value, &i) == V3_OK)
        return StatusCode::OK;
    return StatusCode::INVALID_PARAMETER_OPERATION;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    double ppqPosition = owner->ump_input_dispatcher.trackContext()->ppqPosition(); // I guess only either of those time options are needed.
    uint16_t flags = owner->processData.process_mode == V3_REALTIME ? V3_EVENT_IS_LIVE : 0; // am I right?
    v3_event evt{static_cast<int32_t>(context.group), sampleOffset, ppqPosition, flags, V3_EVENT_NOTE_EXP_VALUE};
    evt.note_exp_value = { .type_id = index, .note_id = static_cast<int32_t>(context.note), .value = value };
    auto evtList = owner->processDataOutputEvents.asInterface();
    // This should copy the argument evt, right?
    evtList->vtable->event_list.add_event(evtList, &evt);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    *value = controller->vtable->controller.get_parameter_normalised(controller, parameter_ids[index]);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
    return StatusCode::NOT_IMPLEMENTED;
}

void remidy::PluginInstanceVST3::ParameterSupport::setProgramChange(remidy::uint4_t group, remidy::uint4_t channel,
                                                                    remidy::uint7_t flags, remidy::uint7_t program,
                                                                    remidy::uint7_t bankMSB, remidy::uint7_t bankLSB) {
    auto unitInfo = owner->unit_info;
    auto states = owner->_states;

    int32_t bank = (bankMSB << 7) + bankLSB;

    v3_program_list_info pl;
    auto result = unitInfo->vtable->unit_info.get_program_list_info(unitInfo, bank, &pl);
    if (result != V3_OK) {
        std::cerr << std::format("Could not retrieve program list: result code: {}, bank: {}, program: {}", result, bank, program) << std::endl;
        return; // could not retrieve program list
    }

    v3_bstream *stream;
    result = unitInfo->vtable->unit_info.set_unit_program_data(unitInfo, pl.id, program, &stream);
    if (result != V3_OK) {
        std::cerr << std::format("Failed to set unit program data: result code: {}, bank: {}, program: {}", result, bank, program) << std::endl;
        return;
    }

    int64_t size;
    stream->seek(stream, 0, V3_SEEK_END, &size);
    std::vector<uint8_t> buf(size);
    int32_t read;
    stream->read(stream, buf.data(), size, &read);
    states->setState(buf, remidy::PluginStateSupport::StateContextType::Preset, true);
}
