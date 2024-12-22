#include "PluginFormatLV2.hpp"

remidy::PluginInstanceLV2::PluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin) :
        PluginInstance(entry), formatImpl(formatImpl), plugin(plugin),
        implContext(formatImpl->worldContext, formatImpl->world, plugin),
        audio_buses(new LV2AudioBuses(this)) {
}

remidy::PluginInstanceLV2::~PluginInstanceLV2() {
    if (instance) {
        lilv_instance_deactivate(instance);
        lilv_instance_free(instance);
    }
    instance = nullptr;

    if (plugin) {
        for (auto p : lv2_ports)
            if (p.port_buffer)
                free(p.port_buffer);
    }

    delete audio_buses;

    delete _parameters;
}

bool getNextAudioPortIndex(remidy_lv2::LV2ImplPluginContext& ctx, const LilvPlugin* plugin, const bool isInput, int32_t& result, int32_t& lv2PortIndex, uint32_t numPorts) {
    while(lv2PortIndex < numPorts) {
        auto port = lilv_plugin_get_port_by_index(plugin, lv2PortIndex);
        if (isInput ? ctx.IS_AUDIO_IN(plugin, port) : ctx.IS_AUDIO_OUT(plugin, port)) {
            result = lv2PortIndex++;
            return true;
        }
        lv2PortIndex++;
    }
    result = -1;
    return false;
}

remidy::StatusCode remidy::PluginInstanceLV2::configure(ConfigurationRequest& configuration) {
    // Do we have to deal with offlineMode? LV2 only mentions hardRT*Capable*.

    if (instance)
        // we need to save state delete instance, recreate instance with the
        // new configuration, and restore the state.
        // FIXME: implement
        throw std::runtime_error("PluginInstanceLV2::configure() re-configuration is not implemented");

    sample_rate = configuration.sampleRate;
    instance = instantiate_plugin(formatImpl->worldContext, &implContext, plugin,
                                  configuration.sampleRate, configuration.offlineMode);
    if (!instance)
        return StatusCode::FAILED_TO_INSTANTIATE;

    for (const auto p : lv2_ports)
        if (p.port_buffer)
            free(p.port_buffer);
    lv2_ports.clear();

    // create port mappings between Remidy and LV2
    uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
    int32_t portToScan = 0;
    auto audioIns = audio_buses->audioInputBuses();
    int32_t lv2AudioInIdx = 0;
    for (size_t i = 0, n = audioIns.size(); i < n; i++) {
        auto bus = audioIns[i];
        for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
            getNextAudioPortIndex(implContext, plugin, true, lv2AudioInIdx, portToScan, numPorts);
            audio_in_port_mapping.emplace_back(RemidyToLV2PortMapping{.bus = i, .channel = ch, .lv2Port = lv2AudioInIdx});
        }
    }
    portToScan = 0;
    const auto audioOuts = audio_buses->audioOutputBuses();
    int32_t lv2AudioOutIdx = 0;
    for (size_t i = 0, n = audioOuts.size(); i < n; i++) {
        const auto bus = audioOuts[i];
        for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
            getNextAudioPortIndex(implContext, plugin, false, lv2AudioOutIdx, portToScan, numPorts);
            audio_out_port_mapping.emplace_back(RemidyToLV2PortMapping{.bus = i, .channel = ch, .lv2Port = lv2AudioOutIdx});
        }
    }

    int32_t nextAtomIn = 0, nextAtomOut = 0;
    for (int i = 0; i < numPorts; i++) {
        const auto port = lilv_plugin_get_port_by_index(plugin, i);

        auto lv2Port = LV2PortInfo{};
        lv2Port.atom_in_index = implContext.IS_ATOM_IN(plugin, port) ? nextAtomIn++ : -1;
        lv2Port.atom_out_index = implContext.IS_ATOM_OUT(plugin, port) ? nextAtomOut++ : -1;

        if (!implContext.IS_AUDIO_PORT(plugin, port)) {
            const LilvNode* minSizeNode = lilv_port_get(plugin, port, implContext.statics->resize_port_minimum_size_node);
            const int minSize = minSizeNode ? lilv_node_as_int(minSizeNode) : 0;
            // If minSize is not specified, it is interpreted as to contain one single float control value.
            lv2Port.buffer_size = minSize ? minSize : sizeof(float);
            auto buffer = calloc(lv2Port.buffer_size, 1);
            lv2Port.port_buffer = buffer;
            lilv_instance_connect_port(instance, i, buffer);

            if (implContext.IS_ATOM_PORT(plugin, port)) {
                lv2_atom_forge_init(&lv2Port.forge, getLV2UridMapData());
                lv2_atom_forge_set_buffer(&lv2Port.forge, (uint8_t*) buffer, lv2Port.buffer_size);
            } else {
                // set default value for (most-likely) ControlPorts.
                auto defaultValue = lilv_port_get(plugin, port, implContext.statics->default_uri_node);
                if (defaultValue)
                    *((float*) lv2Port.port_buffer) = lilv_node_as_float(defaultValue);
            }
        }
        else
            lv2Port.buffer_size = configuration.bufferSizeInSamples * sizeof(float);

        lv2_ports.emplace_back(lv2Port);
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceLV2::startProcessing() {
    if (!instance)
        return StatusCode::ALREADY_INVALID_STATE;
    lilv_instance_activate(instance);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceLV2::stopProcessing() {
    if (!instance)
        return StatusCode::ALREADY_INVALID_STATE;
    lilv_instance_deactivate(instance);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceLV2::process(AudioProcessContext &process) {
    // FIXME: is there 64-bit float audio support?
    for (auto& m : audio_in_port_mapping) {
        auto audioIn = process.getFloatInBuffer(m.bus, m.channel);
        lilv_instance_connect_port(instance, m.lv2Port, audioIn);
    }
    for (auto& m : audio_out_port_mapping) {
        auto audioOut = process.getFloatOutBuffer(m.bus, m.channel);
        lilv_instance_connect_port(instance, m.lv2Port, audioOut);
    }

    for (auto & port : lv2_ports)
        if (port.atom_in_index >= 0 || port.atom_out_index >= 0) {
            lv2_atom_forge_init(&port.forge, getLV2UridMapData());
            lv2_atom_forge_set_buffer(&port.forge, (uint8_t*) port.port_buffer, port.buffer_size);
            lv2_atom_sequence_clear((LV2_Atom_Sequence*) port.port_buffer);
        }

    // FIXME: pass correct timestamp
    ump_input_dispatcher.process(0, process);

    lilv_instance_run(instance, process.frameCount());

    // FIXME: process Atom outputs

    return StatusCode::OK;
}

remidy::PluginParameterSupport *remidy::PluginInstanceLV2::parameters() {
    if (!_parameters)
        _parameters = new ParameterSupport(this);
    return _parameters;
}
