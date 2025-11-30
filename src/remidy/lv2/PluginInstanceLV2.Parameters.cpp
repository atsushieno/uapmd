#include "PluginFormatLV2.hpp"
#include <cmath>
#include <limits>

using namespace remidy;

remidy::PluginInstanceLV2::ParameterSupport::~ParameterSupport()  {
    for (auto h : parameter_handlers)
        delete h;
    for (auto p : parameter_defs)
        delete p;
}

std::vector<remidy::PluginParameter*>& remidy::PluginInstanceLV2::ParameterSupport::parameters() {
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
    double minValue = minValueNode ? lilv_node_as_float(minValueNode) : 0;
    auto maxValueNode = lilv_world_get(implContext.world, parameter, implContext.statics->maximum_uri_node, nullptr);
    double maxValue = maxValueNode ? lilv_node_as_float(maxValueNode) : 0;
    auto enumerationNode = lilv_world_get(implContext.world, parameter, implContext.statics->enumeration_uri_node, nullptr);
    bool isDiscreteEnum = enumerationNode != nullptr && lilv_node_as_bool(enumerationNode);

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

    return std::make_unique<PluginParameter>(index, label, label, portGroup, defValue, minValue, maxValue,
                                             // do we need more checks for automatability?
                                             true, true, false, isDiscreteEnum, enums);
}

void remidy::PluginInstanceLV2::ParameterSupport::inspectParameters() {
    auto formatImpl = owner->formatImpl;
    auto& implContext = owner->implContext;
    auto plugin = owner->plugin;
    auto& displayName = owner->info()->displayName();

    auto logger = formatImpl->worldContext->logger;

    std::vector<std::pair<const LilvNode*,std::unique_ptr<PluginParameter>>> pl{};
    auto mapFeature = &owner->implContext.statics->features.urid_map_feature_data;
    auto mapNodeToUrid = [&](const LilvNode* node, PluginParameter* parameter) {
        if (!node || !parameter)
            return;
        const LilvNode* propertyNode = nullptr;
        auto propertyKey = owner->formatImpl->worldContext->patch_property_uri_node;
        if (propertyKey)
            propertyNode = lilv_world_get(implContext.world, node, propertyKey, nullptr);
        const LilvNode* uriNode = propertyNode ? propertyNode : node;
        if (!uriNode || !lilv_node_is_uri(uriNode))
            return;
        if (!mapFeature->map || !mapFeature->handle)
            return;
        const char* uri = lilv_node_as_uri(uriNode);
        if (!uri)
            return;
        auto urid = mapFeature->map(mapFeature->handle, uri);
        if (urid != 0) {
            property_urid_to_index[urid] = parameter->index();
            index_to_property_urid[parameter->index()] = urid;
        }
    };
    // this is what Ardour does: https://github.com/Ardour/ardour/blob/a76afae0e9ffa8a44311d6f9c1d8dbc613bfc089/libs/ardour/lv2_plugin.cc#L2142
    auto pluginSubject = lilv_plugin_get_uri(plugin);
    auto writables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_writable_uri_node, nullptr);
    int32_t index = 0;
    LILV_FOREACH(nodes, iter, writables) {
        auto node = lilv_nodes_get(writables, iter);
        auto parameter = createParameter(index++, node, implContext, logger, displayName);
        if (parameter)
            pl.emplace_back(std::pair{node, std::move(parameter)});
    }
    // iterate through readable patches. If there is a read-only parameter, add to the list.
    auto readables = lilv_world_find_nodes(formatImpl->world, pluginSubject, formatImpl->worldContext->patch_readable_uri_node, nullptr);
    LILV_FOREACH(nodes, iter, readables) {
        auto node = lilv_nodes_get(writables, iter);
        //auto symbol = lilv_world_get_symbol(owner->implContext.world, node);
        auto w = std::find_if(pl.begin(), pl.end(), [&](const auto &item) { return lilv_node_as_string(item.first) == lilv_node_as_string(node); });
        if (w != pl.end())
            w->second->readable(true);
        else {
            auto parameter = createParameter(index++, node, implContext, logger, displayName);
            if (parameter)
                pl.emplace_back(std::pair{node, std::move(parameter)});
        }
    }

    for (auto& p : pl) {
        auto para = p.second.release();
        parameter_handlers.emplace_back(new LV2AtomParameterHandler(this, implContext, para));
        parameter_defs.emplace_back(para);
        mapNodeToUrid(p.first, para);
    }
}

remidy::StatusCode remidy::PluginInstanceLV2::ParameterSupport::getParameter(uint32_t index, double *value) {
    return parameter_handlers[index]->getParameter(value);
}

remidy::StatusCode remidy::PluginInstanceLV2::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // Public API - called from host, should notify UI
    return setParameterInternal(index, value, timestamp, true);
}

remidy::StatusCode remidy::PluginInstanceLV2::ParameterSupport::setParameterInternal(uint32_t index, double value, uint64_t timestamp, bool notifyUI) {
    auto status = parameter_handlers[index]->setParameter(value, timestamp);
    if (status == StatusCode::OK) {
        notifyParameterValue(static_cast<uint32_t>(index), value);

        if (notifyUI) {
            auto* lv2UI = dynamic_cast<PluginInstanceLV2::UISupport*>(owner->ui());
            if (lv2UI) {
                if (auto propertyUrid = propertyUridForIndex(static_cast<uint32_t>(index)); propertyUrid.has_value())
                    lv2UI->notifyParameterChange(propertyUrid.value(), value);
            }
        }
    }
    return status;
}

std::string PluginInstanceLV2::ParameterSupport::valueToString(uint32_t index, double value) {
    const auto& enums = parameter_defs[index]->enums();
    if (enums.empty())
        return "";

    const double tolerance = 1e-6;
    const ParameterEnumeration* bestMatch = nullptr;
    double bestDiff = std::numeric_limits<double>::max();

    for (const auto& e : enums) {
        double diff = std::abs(e.value - value);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestMatch = &e;
        }
    }

    if (bestMatch == nullptr)
        return "";

    if (bestDiff <= tolerance)
        return bestMatch->label;

    if (value <= enums.front().value)
        return enums.front().label;
    if (value >= enums.back().value)
        return enums.back().label;

    return bestMatch->label;
}

void remidy::PluginInstanceLV2::ParameterSupport::refreshParameterMetadata(uint32_t index) {
    if (index >= parameter_defs.size())
        return;

    auto& implContext = owner->implContext;
    auto param = parameter_defs[index];

    // Find the LilvNode for this parameter by iterating through patch:writable
    auto pluginSubject = lilv_plugin_get_uri(owner->plugin);
    auto writables = lilv_world_find_nodes(implContext.world, pluginSubject, owner->formatImpl->worldContext->patch_writable_uri_node, nullptr);

    uint32_t currentIndex = 0;
    LILV_FOREACH(nodes, iter, writables) {
        if (currentIndex == index) {
            auto node = lilv_nodes_get(writables, iter);

            // Re-query min/max/default values
            auto defValueNode = lilv_world_get(implContext.world, node, implContext.statics->default_uri_node, nullptr);
            double defValue = defValueNode ? lilv_node_as_float(defValueNode) : param->defaultPlainValue();
            auto minValueNode = lilv_world_get(implContext.world, node, implContext.statics->minimum_uri_node, nullptr);
            double minValue = minValueNode ? lilv_node_as_float(minValueNode) : param->minPlainValue();
            auto maxValueNode = lilv_world_get(implContext.world, node, implContext.statics->maximum_uri_node, nullptr);
            double maxValue = maxValueNode ? lilv_node_as_float(maxValueNode) : param->maxPlainValue();

            param->updateRange(minValue, maxValue, defValue);
            break;
        }
        currentIndex++;
    }
    lilv_nodes_free(writables);
}

std::optional<uint32_t> remidy::PluginInstanceLV2::ParameterSupport::indexForProperty(LV2_URID propertyUrid) const {
    auto it = property_urid_to_index.find(propertyUrid);
    if (it == property_urid_to_index.end())
        return std::nullopt;
    return it->second;
}

std::optional<LV2_URID> remidy::PluginInstanceLV2::ParameterSupport::propertyUridForIndex(uint32_t index) const {
    auto it = index_to_property_urid.find(index);
    if (it == index_to_property_urid.end())
        return std::nullopt;
    return it->second;
}

void remidy::PluginInstanceLV2::ParameterSupport::updateCachedParameterValue(uint32_t index, double plainValue) {
    if (index >= parameter_handlers.size())
        return;
    parameter_handlers[index]->updateCachedValue(plainValue);
}
