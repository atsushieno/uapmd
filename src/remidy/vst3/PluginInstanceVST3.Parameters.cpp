
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

remidy::AudioPluginInstanceVST3::ParameterSupport::ParameterSupport(AudioPluginInstanceVST3* owner) : owner(owner) {
    auto controller = owner->controller;
    auto count = controller->vtable->controller.get_parameter_count(controller);

    parameter_ids = (v3_param_id*) calloc(sizeof(v3_param_id), count);

    for (auto i = 0; i < count; i++) {
        v3_param_info info{};
        controller->vtable->controller.get_parameter_info(controller, i, &info);
        std::string idString = std::format("{}", info.param_id);
        std::string name = vst3StringToStdString(info.title);
        std::string path{""};

        auto p = new PluginParameter(info.param_id, idString, name, path, info.default_normalised_value, 0, 1, true, info.flags & V3_PARAM_IS_HIDDEN);
        parameter_ids[i] = info.param_id;
        parameter_defs.emplace_back(p);
    }
}

remidy::AudioPluginInstanceVST3::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_defs)
        delete p;
    for (auto pair : per_note_controller_defs)
        for (auto p : pair.second)
            delete p;
    free(parameter_ids);
}

std::vector<remidy::PluginParameter*>& remidy::AudioPluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

std::vector<remidy::PluginParameter*>& remidy::AudioPluginInstanceVST3::ParameterSupport::perNoteControllers(
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
                    true, false);
                defs.emplace_back(parameter);
            }
        }
    }
    const auto ctx = PerNoteControllerContext{0, context.channel, context.group, 0};
    per_note_controller_defs.emplace_back(ctx, defs);
    return perNoteControllers(types, ctx);
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // use IParameterChanges.
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    auto pvc = owner->processDataInputParameterChanges.asInterface();
    const v3_param_id dummy = index;
    int32_t i = 0;
    IParamValueQueue* q{nullptr};
    for (int32_t n = pvc->vtable->parameter_changes.get_param_count(pvc); i < n; i++) {
        q = reinterpret_cast<IParamValueQueue *>(pvc->vtable->parameter_changes.get_param_data(pvc, i));
        if (q->vtable->param_value_queue.get_param_id(q) == index)
            break;
    }
    if (!q)
        // It is RT-safe operation, right?
        q = reinterpret_cast<IParamValueQueue *>(pvc->vtable->parameter_changes.add_param_data(pvc, &dummy, &i));
    if (q && q->vtable->param_value_queue.add_point(q, sampleOffset, value, &i) == V3_OK)
        return StatusCode::OK;
    return StatusCode::INVALID_PARAMETER_OPERATION;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
    int32_t sampleOffset = 0; // FIXME: calculate from timestamp
    double ppqPosition = owner->ump_input_dispatcher.trackContext()->ppqPosition(); // I guess only either of those time options are needed.
    uint16_t flags = owner->processData.process_mode == V3_REALTIME ? V3_EVENT_IS_LIVE : 0; // am I right?
    v3_event evt{static_cast<int32_t>(context.group), sampleOffset, ppqPosition, flags, V3_EVENT_NOTE_EXP_VALUE};
    evt.note_exp_value = {.value = value, .type_id = index, .note_id = static_cast<int32_t>(context.note)};
    auto evtList = owner->processDataOutputEvents.asInterface();
    // This should copy the argument evt, right?
    evtList->vtable->event_list.add_event(evtList, &evt);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::getParameter(uint32_t index, double* value) {
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    *value = controller->vtable->controller.get_parameter_normalised(controller, parameter_ids[index]);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
    return StatusCode::NOT_IMPLEMENTED;
}