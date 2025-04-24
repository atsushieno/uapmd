#include "PluginFormatLV2.hpp"

using namespace remidy;

remidy::PluginInstanceLV2::ParameterSupport::~ParameterSupport()  {
    for (auto h : parameter_handlers)
        delete h;
    for (auto p : parameter_defs)
        delete p;
}

std::vector<remidy::PluginParameter*> remidy::PluginInstanceLV2::ParameterSupport::parameters() {
    return parameter_defs;
}

std::unique_ptr<PluginParameter> createParameter(uint32_t index, const LilvNode* parameter, remidy_lv2::LV2ImplPluginContext& implContext, remidy::Logger* logger, std::string& displayName) {
    auto labelNode = lilv_world_get(implContext.world, parameter, implContext.statics->rdfs_label_node, nullptr);
    if (!labelNode) {
        logger->logError("A patch writable does not specify RDF label.");
        return nullptr;
    }
    auto label = std::string{lilv_node_as_string(labelNode)};

    // portGroup is used as its parameter path
    auto portGroupNode = lilv_world_get(implContext.world, parameter, implContext.statics->port_group_uri_node, nullptr);
    auto portGroup = portGroupNode ? std::string{lilv_node_as_string(portGroupNode)} : "";

    auto rangeNode = lilv_world_get(implContext.world, parameter, implContext.statics->rdfs_range_node, nullptr);
    if (rangeNode) {
        auto type = std::string{lilv_node_as_uri(rangeNode)};
        if (type == LV2_ATOM__Float ||
            type == LV2_ATOM__Double ||
            type == LV2_ATOM__Bool ||
            type == LV2_ATOM__Int ||
            type == LV2_ATOM__Long)
            ;// okay
        else if (type == LV2_ATOM__String ||
                 type == LV2_ATOM__Path ||
                 type == LV2_ATOM__URI) {
            logger->logInfo("%s: ATOM String, Path, and URI are ignored.", displayName.c_str());
            return nullptr;
        }
        else {
            logger->logError("%s: Unexpected ATOM type `%s`. Ignored.", displayName.c_str(), type.c_str());
            return nullptr;
        }
    }

    // There is no `lilv_node_as_double()` (no `LILV_VALUE_DOUBLE` either...)
    auto defValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->default_uri_node, nullptr);
    double defValue = defValueNode ? lilv_node_as_float(defValueNode) : 0;
    auto minValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->minimum_uri_node, nullptr);
    double minValue = defValueNode ? lilv_node_as_float(minValueNode) : 0;
    auto maxValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->maximum_uri_node, nullptr);
    double maxValue = defValueNode ? lilv_node_as_float(maxValueNode) : 0;

    std::vector<ParameterEnumeration> enums{};
    auto scalePoints = lilv_world_find_nodes(implContext.world, parameter, implContext.statics->scale_point_uri_node, nullptr);
    LILV_FOREACH(nodes, s, scalePoints) {
        auto sv = lilv_nodes_get(scalePoints, s);
        auto enumValueNode = lilv_world_get(implContext.world, sv,
                                            implContext.statics->rdf_value_node, nullptr);
        auto enumValue = enumValueNode ? lilv_node_as_float(enumValueNode) : 0;
        auto enumLabelNode = lilv_world_get(implContext.world, sv,
                                            implContext.statics->rdfs_label_node, nullptr);
        if (!enumLabelNode)
            // warn about missing label, but continue, it might be anonymous.
            logger->logWarning("%s: A scalePoint for `%s` misses its label at %f.",
                               displayName.c_str(), label.c_str(), enumValue);
        auto enumLabel = std::string{lilv_node_as_string(enumLabelNode)};
        ParameterEnumeration pe{enumLabel, enumValue};
        enums.emplace_back(pe);
    }

    return std::make_unique<PluginParameter>(index, label, label, portGroup, defValue, minValue, maxValue, false, false, enums);
}

void remidy::PluginInstanceLV2::ParameterSupport::inspectParameters() {
    auto formatImpl = owner->formatImpl;
    auto& implContext = owner->implContext;
    auto plugin = owner->plugin;
    auto& displayName = owner->info()->displayName();

    auto logger = formatImpl->worldContext->logger;

    std::map<const LilvNode*,std::unique_ptr<PluginParameter>> pl{};
    // this is what Ardour does: https://github.com/Ardour/ardour/blob/a76afae0e9ffa8a44311d6f9c1d8dbc613bfc089/libs/ardour/lv2_plugin.cc#L2142
    auto pluginSubject = lilv_plugin_get_uri(plugin);
    auto writables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_writable_uri_node, nullptr);
    uint32_t index = 0;
    LILV_FOREACH(nodes, iter, writables) {
        auto writable = lilv_nodes_get(writables, iter);
        auto parameter = createParameter(index, writable, implContext, logger, displayName);
        if (parameter)
            pl[writable] = std::move(parameter);
        index++;
    }
    // iterate through readable patches. If there is a read-only parameter, add to the list.
    auto readables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_readable_uri_node, nullptr);
    LILV_FOREACH(nodes, iter, readables) {
        auto readable = lilv_nodes_get(writables, iter);
        if (pl.contains(readable))
            pl[readable]->readable(true);
        else {
            auto parameter = createParameter(index, readable, implContext, logger, displayName);
            if (parameter)
                pl[readable] = std::move(parameter);
            index++;
        }
    }

    for (auto& p : pl) {
        auto para = p.second.release();
        parameter_handlers.emplace_back(new LV2AtomParameterHandler(this, implContext, para));
        parameter_defs.emplace_back(para);
    }
}

remidy::StatusCode remidy::PluginInstanceLV2::ParameterSupport::getParameter(int32_t note, uint32_t index, double *value) {
    if (note >= 0) {
        owner->formatImpl->getLogger()->logError("LV2 does not support per-note parameters.");
        return StatusCode::OK;
    }
    return parameter_handlers[index]->getParameter(value);
}

remidy::StatusCode remidy::PluginInstanceLV2::ParameterSupport::setParameter(int32_t note, uint32_t index, double value, uint64_t timestamp) {
    if (note >= 0) {
        owner->formatImpl->getLogger()->logError("LV2 does not support per-note parameters.");
        return StatusCode::OK;
    }
    return parameter_handlers[index]->setParameter(value, timestamp);
}
