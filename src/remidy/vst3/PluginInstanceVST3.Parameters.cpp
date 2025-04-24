
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
    free(parameter_ids);
}

std::vector<remidy::PluginParameter*> remidy::AudioPluginInstanceVST3::ParameterSupport::parameters() {
    return parameter_defs;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::setParameter(int32_t note, uint32_t index, double value, uint64_t timestamp) {
    // FIXME: use IParamValueChanges for sample-accurate parameter changes
    // FIXME: support per-note controllers.
    if (note >= 0) {
        owner->owner->getLogger()->logError("Per-note setParameter() on VST3 is not implemented.");
        return StatusCode::OK;
    }

    auto controller = owner->controller;
    controller->vtable->controller.set_parameter_normalised(controller, parameter_ids[index], value);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::ParameterSupport::getParameter(int32_t note, uint32_t index, double* value) {
    if (note >= 0) {
        owner->owner->getLogger()->logError("Per-note getParameter() on VST3 is not implemented.");
        return StatusCode::OK;
    }

    // FIXME: support per-note controllers.
    if (!value)
        return StatusCode::INVALID_PARAMETER_OPERATION;
    auto controller = owner->controller;
    *value = controller->vtable->controller.get_parameter_normalised(controller, parameter_ids[index]);
    return StatusCode::OK;
}
