#include <format>
#include "PluginFormatCLAP.hpp"

namespace remidy {
    PluginInstanceCLAP::ParameterSupport::ParameterSupport(PluginInstanceCLAP* owner) : owner(owner) {
    }

    PluginInstanceCLAP::ParameterSupport::~ParameterSupport() {
        for (auto p : parameter_defs)
            delete p;
        parameter_defs.clear();
    }

    std::vector<PluginParameter*>& PluginInstanceCLAP::ParameterSupport::parameters() {
        if (!parameter_defs.empty())
            return parameter_defs;

        EventLoop::runTaskOnMainThread([&] {
            const auto plugin = owner->plugin;
            params_ext = (clap_plugin_params_t*) plugin->get_extension(plugin, CLAP_EXT_PARAMS);
            for (size_t i = 0, n = params_ext->count(plugin); i < n; i++) {
                clap_param_info_t info;
                if (!params_ext->get_info(plugin, i, &info))
                    continue;
                std::vector<ParameterEnumeration> enums{};
                std::string id{std::format("{}", info.id)};
                std::string name{info.name};
                std::string module{info.module};

                if (info.flags & CLAP_PARAM_IS_ENUM && info.flags & CLAP_PARAM_IS_STEPPED) {
                    char enumLabel[1024];
                    for (int i = 0; i < info.max_value; i++) {
                        if (params_ext->value_to_text(plugin, info.id, i, enumLabel, sizeof(enumLabel))) {
                            std::string enumLabelString{enumLabel};
                            enums.emplace_back(enumLabelString, i);
                        }
                    }
                }

                parameter_defs.emplace_back(new PluginParameter(
                        i,
                        id,
                        name,
                        module,
                        info.default_value,
                        info.min_value,
                        info.max_value,
                        true,
                        info.flags & CLAP_PARAM_IS_HIDDEN,
                        info.flags & CLAP_PARAM_IS_ENUM,
                        enums));
                parameter_ids.emplace_back(info.id);
                parameter_cookies.emplace_back(info.cookie);
            }
        });
        return parameter_defs;
    }

    std::vector<PluginParameter*>& PluginInstanceCLAP::ParameterSupport::perNoteControllers(
        PerNoteControllerContextTypes types,
        PerNoteControllerContext context
    ) {
        // CLAP has no distinct definitions for parameters and per-note controllers.
        return parameter_defs;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
        auto a = owner->events.tryAllocate(alignof(void *), sizeof(clap_event_param_value_t));
        if (!a)
            return StatusCode::INSUFFICIENT_MEMORY;
        const auto evt = reinterpret_cast<clap_event_param_value_t *>(a);
        evt->header.type = CLAP_EVENT_PARAM_VALUE;
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.flags = CLAP_EVENT_IS_LIVE;
        // FIXME: assign timestamp *in samples*
        //evt->header.time = timestamp;
        evt->cookie = parameter_cookies[index];
        evt->port_index = 0;
        evt->channel = 0;
        evt->param_id = parameter_ids[index];
        evt->value = value;

        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
        auto evt = reinterpret_cast<clap_event_param_value_t *>(owner->events.tryAllocate(alignof(void *),
            sizeof(clap_event_param_value_t)));
        evt->header.type = CLAP_EVENT_PARAM_VALUE;
        evt->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        evt->header.flags |= CLAP_EVENT_IS_LIVE;
        // FIXME: calculate timestamp (to samples)
        //evt->header.time = timestamp;
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

        return StatusCode::NOT_IMPLEMENTED;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::getPerNoteController(PerNoteControllerContext context, uint32_t index, double *value) {
        return StatusCode::NOT_IMPLEMENTED;
    }
}