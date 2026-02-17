#include "PluginFormatAAP.hpp"

remidy::PluginUIThreadRequirement remidy::PluginInstanceAAP::requiresUIThreadOn() {
    return InstanceControl;
}

remidy::StatusCode
remidy::PluginInstanceAAP::configure(remidy::PluginInstance::ConfigurationRequest &configuration) {
    return StatusCode::ALREADY_INSTANTIATED;
}

remidy::StatusCode remidy::PluginInstanceAAP::startProcessing() {
    return StatusCode::ALREADY_INSTANTIATED;
}

remidy::StatusCode remidy::PluginInstanceAAP::stopProcessing() {
    return StatusCode::ALREADY_INSTANTIATED;
}

remidy::StatusCode remidy::PluginInstanceAAP::process(remidy::AudioProcessContext &process) {
    return StatusCode::ALREADY_INSTANTIATED;
}

remidy::PluginAudioBuses *remidy::PluginInstanceAAP::audioBuses() {
    return nullptr;
}

remidy::PluginParameterSupport *remidy::PluginInstanceAAP::parameters() {
    return nullptr;
}

remidy::PluginStateSupport *remidy::PluginInstanceAAP::states() {
    return nullptr;
}

remidy::PluginPresetsSupport *remidy::PluginInstanceAAP::presets() {
    return nullptr;
}

remidy::PluginUISupport *remidy::PluginInstanceAAP::ui() {
    return nullptr;
}
