#include <format>
#include "PluginFormatCLAP.hpp"

namespace remidy {
    PluginInstanceCLAP::ParameterSupport::ParameterSupport(PluginInstanceCLAP* owner) : owner(owner) {
    }

    PluginInstanceCLAP::ParameterSupport::~ParameterSupport() {
        clearParameterList();
    }

    void PluginInstanceCLAP::ParameterSupport::clearParameterList() {
        for (auto* param : parameter_defs)
            delete param;
        parameter_defs.clear();
        parameter_ids.clear();
        parameter_cookies.clear();
        parameter_flags.clear();
        param_id_to_index.clear();
        per_note_parameter_defs.clear();
    }

    void PluginInstanceCLAP::ParameterSupport::populateParameterList(bool force) {
        EventLoop::runTaskOnMainThread([&] {
            if (!owner->plugin || !owner->plugin->canUseParams())
                return;
            if (!force && !parameter_defs.empty())
                return;

            clearParameterList();

            const size_t count = owner->plugin->paramsCount();
            parameter_defs.reserve(count);
            parameter_ids.reserve(count);
            parameter_cookies.reserve(count);
            parameter_flags.reserve(count);

            for (size_t i = 0; i < count; ++i) {
                clap_param_info_t info{};
                if (!owner->plugin->paramsGetInfo(static_cast<uint32_t>(i), &info))
                    continue;
                std::vector<ParameterEnumeration> enums{};
                std::string id{std::format("{}", info.id)};
                std::string name{info.name};
                std::string module{info.module};

                // Some plugins only have CLAP_PARAM_IS_STEPPED == true instead of CLAP_PARAM_IS_ENUM == true (e.g. those from clap-juce-extensions)...
                bool isEnum = (info.flags & CLAP_PARAM_IS_ENUM) || (info.flags & CLAP_PARAM_IS_STEPPED);
                if (isEnum) {
                    char enumLabel[1024];
                    const double vmin = info.min_value;
                    const double vmax = info.max_value;
                    const double span = vmax - vmin;
                    if (span > 0.0) {
                        const int probes = 512; // dense enough to capture common step counts
                        std::string lastLabel;
                        for (int p = 0; p <= probes; ++p) {
                            const double t = static_cast<double>(p) / static_cast<double>(probes);
                            const double val = vmin + span * t;
                            if (owner->plugin->paramsValueToText(info.id, val, enumLabel, sizeof(enumLabel))) {
                                std::string lbl{enumLabel};
                                if (!lbl.empty() && (enums.empty() || lbl != lastLabel)) {
                                    enums.emplace_back(lbl, val);
                                    lastLabel = lbl;
                                }
                            }
                        }
                    }
                }

                parameter_defs.emplace_back(new PluginParameter(
                        static_cast<uint32_t>(i),
                        id,
                        name,
                        module,
                        info.default_value,
                        info.min_value,
                        info.max_value,
                        true,
                        info.flags & CLAP_PARAM_IS_AUTOMATABLE,
                        info.flags & CLAP_PARAM_IS_HIDDEN,
                        isEnum,
                        enums));
                parameter_ids.emplace_back(info.id);
                parameter_cookies.emplace_back(info.cookie);
                parameter_flags.emplace_back(info.flags);
                param_id_to_index[info.id] = static_cast<uint32_t>(i);
            }
        });
    }

    void PluginInstanceCLAP::ParameterSupport::broadcastAllParameterValues() {
        auto& defs = parameters();
        for (size_t i = 0; i < defs.size(); ++i) {
            double value = 0.0;
            if (getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK)
                parameterChangeEvent().notify(static_cast<uint32_t>(i), value);
        }
    }

    void PluginInstanceCLAP::ParameterSupport::refreshAllParameterMetadata() {
        populateParameterList(true);
        broadcastAllParameterValues();
        parameterMetadataChangeEvent().notify();
    }

    std::vector<PluginParameter*>& PluginInstanceCLAP::ParameterSupport::parameters() {
        if (parameter_defs.empty())
            populateParameterList(false);
        return parameter_defs;
    }

    std::vector<PluginParameter*>& PluginInstanceCLAP::ParameterSupport::perNoteControllers(
        PerNoteControllerContextTypes types,
        PerNoteControllerContext context
    ) {
        (void) context;
        if (parameter_defs.empty())
            parameters();

        if (types == PER_NOTE_CONTROLLER_NONE)
            return parameter_defs;

        per_note_parameter_defs.clear();
        per_note_parameter_defs.reserve(parameter_defs.size());
        for (size_t i = 0; i < parameter_defs.size(); ++i) {
            if (parameterSupportsContext(parameter_flags[i], types))
                per_note_parameter_defs.emplace_back(parameter_defs[i]);
        }
        return per_note_parameter_defs;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
        if (index >= parameter_ids.size() || index >= parameter_cookies.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;
        auto a = owner->events_in->tryAllocate(alignof(void *), sizeof(clap_event_param_value_t));
        if (!a)
            return StatusCode::INSUFFICIENT_MEMORY;
        const auto evt = reinterpret_cast<clap_event_param_value_t *>(a);
        evt->header.type = CLAP_EVENT_PARAM_VALUE;
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.flags = CLAP_EVENT_IS_LIVE;
        // Convert timestamp from nanoseconds to samples
        evt->header.time = static_cast<uint32_t>((timestamp * owner->sample_rate_) / 1000000000.0);
        evt->cookie = parameter_cookies[index];
        evt->port_index = 0;
        evt->channel = 0;
        evt->param_id = parameter_ids[index];
        evt->value = value;

        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
        if (index >= parameter_ids.size() || index >= parameter_cookies.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;
        auto evt = reinterpret_cast<clap_event_param_value_t *>(owner->events_in->tryAllocate(alignof(void *),
                                                                                              sizeof(clap_event_param_value_t)));
        evt->header.type = CLAP_EVENT_PARAM_VALUE;
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.flags |= CLAP_EVENT_IS_LIVE;
        // Convert timestamp from nanoseconds to samples
        evt->header.time = static_cast<uint32_t>((timestamp * owner->sample_rate_) / 1000000000.0);
        evt->cookie = parameter_cookies[index];
        evt->port_index = context.group;
        evt->channel = context.channel;
        evt->param_id = index;
        evt->key = context.note;
        evt->value = value;

        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::getParameter(uint32_t index, double* value) {
        if (!value)
            return StatusCode::INVALID_PARAMETER_OPERATION;
        if (!owner->plugin || !owner->plugin->canUseParams())
            return StatusCode::NOT_IMPLEMENTED;
        if (index >= parameter_ids.size())
            return StatusCode::INVALID_PARAMETER_OPERATION;

        double queriedValue = 0.0;
        if (!owner->plugin->paramsGetValue(parameter_ids[index], &queriedValue))
            return StatusCode::INVALID_PARAMETER_OPERATION;

        *value = queriedValue;
        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
        return StatusCode::NOT_IMPLEMENTED;
    }

    std::string PluginInstanceCLAP::ParameterSupport::valueToString(uint32_t index, double value) {
        if (!owner->plugin || !owner->plugin->canUseParams() || index >= parameter_ids.size())
            return "";
        char s[1024];
        return owner->plugin->paramsValueToText(parameter_ids[index], value, s, sizeof(s)) ? s : "";
    }

    std::string PluginInstanceCLAP::ParameterSupport::valueToStringPerNote(PerNoteControllerContext context, uint32_t index, double value) {
        (void) context;
        return valueToString(index, value);
    }

    void PluginInstanceCLAP::ParameterSupport::refreshParameterMetadata(uint32_t index) {
        if (index >= parameter_defs.size() || !owner->plugin || !owner->plugin->canUseParams())
            return;

        clap_param_info_t info;
        if (owner->plugin->paramsGetInfo(index, &info)) {
            parameter_defs[index]->updateRange(info.min_value, info.max_value, info.default_value);
        }
    }

    bool PluginInstanceCLAP::ParameterSupport::parameterSupportsContext(uint32_t flags, PerNoteControllerContextTypes types) const {
        if (types & PER_NOTE_CONTROLLER_PER_GROUP) {
            if ((flags & CLAP_PARAM_IS_MODULATABLE_PER_PORT) == 0)
                return false;
        }
        if (types & PER_NOTE_CONTROLLER_PER_CHANNEL) {
            if ((flags & CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL) == 0)
                return false;
        }
        if (types & PER_NOTE_CONTROLLER_PER_NOTE) {
            const bool supportsKey = (flags & CLAP_PARAM_IS_MODULATABLE_PER_KEY) != 0;
            const bool supportsNoteId = (flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) != 0;
            if (!supportsKey && !supportsNoteId)
                return false;
        }
        return true;
    }

    std::optional<uint32_t> PluginInstanceCLAP::ParameterSupport::indexForParamId(clap_id id) const {
        auto it = param_id_to_index.find(id);
        if (it == param_id_to_index.end())
            return std::nullopt;
        return it->second;
    }

    void PluginInstanceCLAP::ParameterSupport::notifyParameterValue(clap_id id, double plainValue) {
        if (auto index = indexForParamId(id); index.has_value())
            parameterChangeEvent().notify(index.value(), plainValue);
    }
}
