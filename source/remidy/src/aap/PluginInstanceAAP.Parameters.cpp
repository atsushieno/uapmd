#include <cstdint>
#include "remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::ParameterSupport::ParameterSupport(PluginInstanceAAP *owner) : owner(owner) {
    auto aap = owner->aapInstance();
    for (auto i = 0, n = aap->getNumParameters(); i < n; i++) {
        auto src = aap->getParameter(i);
        std::vector<remidy::ParameterEnumeration> enums{};
        for (auto e = 0, ne = src->getEnumCount(); e < ne; e++) {
            auto en = src->getEnumeration(e);
            std::string name{en.getName()};
            remidy::ParameterEnumeration ed{name, en.getValue()};
            enums.push_back(ed);
        }
        std::string id = std::to_string(src->getId());
        std::string name = src->getName();
        std::string path = "";
        parameter_list.push_back(new PluginParameter(i, id, name, path,
                                                         src->getDefaultValue(),
                                                         src->getMinimumValue(),
                                                         src->getMaximumValue(),
                                                         true,
                                                         true,
                                                         false,
                                                         src->getEnumCount() > 0,
                                                         enums));

        // AAP does not have per-note controller yet.
    }
}

remidy::PluginInstanceAAP::ParameterSupport::~ParameterSupport() {
    for (auto p : parameter_list)
        delete p;
    for (auto p : per_note_controller_list)
        delete p;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::setParameter(uint32_t index, double plainValue,
                                                uint64_t timestamp) {
    // FIXME: implement
    return StatusCode::OK;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::getParameter(uint32_t index, double *plainValue) {
    // FIXME: implement
    *plainValue = 0;
    return StatusCode::OK;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::setPerNoteController(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double value,
                                                        uint64_t timestamp) {
    // FIXME: implement
    return StatusCode::OK;
}

remidy::StatusCode
remidy::PluginInstanceAAP::ParameterSupport::getPerNoteController(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double *value) {
    // FIXME: implement
    *value = 0;
    return StatusCode::OK;
}

std::string remidy::PluginInstanceAAP::ParameterSupport::valueToString(uint32_t index, double value) {
    // FIXME: implement
    return "0";
}

std::string
remidy::PluginInstanceAAP::ParameterSupport::valueToStringPerNote(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double value) {
    // FIXME: implement
    return "";
}
