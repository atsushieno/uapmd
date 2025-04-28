#include "PluginFormatCLAP.hpp"

namespace remidy {
    PluginInstanceCLAP::ParameterSupport::ParameterSupport(PluginInstanceCLAP* owner) : owner(owner) {
    }

    PluginInstanceCLAP::ParameterSupport::~ParameterSupport() {
    }

    std::vector<PluginParameter*>& PluginInstanceCLAP::ParameterSupport::parameters() {
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
        return StatusCode::NOT_IMPLEMENTED;
    }

    StatusCode PluginInstanceCLAP::ParameterSupport::setPerNoteController(PerNoteControllerContext context, uint32_t index, double value, uint64_t timestamp) {
        return StatusCode::NOT_IMPLEMENTED;
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