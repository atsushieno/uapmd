#include <cstdint>
#include "remidy.hpp"
#include <aap/plugin-meta-info.h>
#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::ParameterSupport::ParameterSupport(PluginInstanceAAP *owner) : owner(owner) {
    // FIXME: fill parameter list here
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
    return StatusCode::OK;
}

std::string remidy::PluginInstanceAAP::ParameterSupport::valueToString(uint32_t index, double value) {
    // FIXME: implement
    return "";
}

std::string
remidy::PluginInstanceAAP::ParameterSupport::valueToStringPerNote(remidy::PerNoteControllerContext context,
                                                        uint32_t index, double value) {
    // FIXME: implement
    return "";
}
