#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::PluginInstanceAAP(
        PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance
) : PluginInstance(entry), format(format), instance(aapInstance) {
}

remidy::StatusCode
remidy::PluginInstanceAAP::configure(remidy::PluginInstance::ConfigurationRequest &configuration) {
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::startProcessing() {
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::stopProcessing() {
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::process(remidy::AudioProcessContext &process) {
    return StatusCode::OK;
}
