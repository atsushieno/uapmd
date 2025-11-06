#include "PluginFormatLV2.hpp"
#include "cmidi2.h"
#include <algorithm>
#include <vector>

remidy::PluginInstanceLV2::PluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2::Impl* formatImpl, const LilvPlugin* plugin) :
        PluginInstance(entry), formatImpl(formatImpl), plugin(plugin),
        implContext(formatImpl->worldContext, formatImpl->world, plugin),
        audio_buses(new AudioBuses(this)) {
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
    delete _states;
    delete _presets;
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

    audio_buses->configure(configuration);
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
    audio_in_port_mapping.clear();
    audio_in_fallback_buffers.clear();
    int32_t lv2AudioInIdx = 0;
    for (size_t i = 0, n = audioIns.size(); i < n; i++) {
        auto bus = audioIns[i];
        for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
            if (!getNextAudioPortIndex(implContext, plugin, true, lv2AudioInIdx, portToScan, numPorts)) {
                formatImpl->getLogger()->logWarning("LV2 plugin %s has fewer input ports than expected", info()->displayName().c_str());
                continue;
            }
            audio_in_port_mapping.emplace_back(RemidyToLV2PortMapping{.bus = i, .channel = ch, .lv2Port = lv2AudioInIdx});
        }
    }
    portToScan = 0;
    const auto audioOuts = audio_buses->audioOutputBuses();
    audio_out_port_mapping.clear();
    audio_out_fallback_buffers.clear();
    int32_t lv2AudioOutIdx = 0;
    for (size_t i = 0, n = audioOuts.size(); i < n; i++) {
        const auto bus = audioOuts[i];
        for (uint32_t ch = 0, nCh = bus->channelLayout().channels(); ch < nCh; ch++) {
            if (!getNextAudioPortIndex(implContext, plugin, false, lv2AudioOutIdx, portToScan, numPorts)) {
                formatImpl->getLogger()->logWarning("LV2 plugin %s has fewer output ports than expected", info()->displayName().c_str());
                continue;
            }
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

    auto ensureFallbackSize = [&](std::vector<std::vector<float>>& buffers, size_t count) {
        buffers.resize(count);
        for (auto& buf : buffers)
            buf.assign(configuration.bufferSizeInSamples, 0.0f);
    };
    ensureFallbackSize(audio_in_fallback_buffers, audio_in_port_mapping.size());
    ensureFallbackSize(audio_out_fallback_buffers, audio_out_port_mapping.size());

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
    for (size_t i = 0; i < audio_in_port_mapping.size(); ++i) {
        auto& m = audio_in_port_mapping[i];
        auto audioIn = process.getFloatInBuffer(m.bus, m.channel);
        if (!audioIn) {
            auto& fallback = audio_in_fallback_buffers[i];
            std::fill(fallback.begin(), fallback.begin() + process.frameCount(), 0.0f);
            audioIn = fallback.data();
        }
        lilv_instance_connect_port(instance, m.lv2Port, audioIn);
    }
    for (size_t i = 0; i < audio_out_port_mapping.size(); ++i) {
        auto& m = audio_out_port_mapping[i];
        auto audioOut = process.getFloatOutBuffer(m.bus, m.channel);
        if (!audioOut) {
            auto& fallback = audio_out_fallback_buffers[i];
            std::fill(fallback.begin(), fallback.begin() + process.frameCount(), 0.0f);
            audioOut = fallback.data();
        }
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

    // Process Atom outputs and convert to UMP
    auto& eventOut = process.eventOut();
    auto* umpBuffer = static_cast<uint64_t*>(eventOut.getMessages());
    size_t umpPosition = eventOut.position() / sizeof(uint64_t);
    size_t umpCapacity = eventOut.maxMessagesInBytes() / sizeof(uint64_t);

    for (auto& port : lv2_ports) {
        if (port.atom_out_index < 0)
            continue;

        auto* seq = (LV2_Atom_Sequence*)port.port_buffer;
        if (!seq)
            continue;

        // Iterate through events in the atom sequence
        LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
            if (umpPosition >= umpCapacity)
                break;

            const LV2_Atom* atom = &ev->body;

            // Check if this is a MIDI event
            if (atom->type == implContext.statics->urids.urid_midi_event_type) {
                // MIDI1 event - convert to MIDI2 UMP
                const uint8_t* midi = (const uint8_t*)(atom + 1);
                uint8_t status = midi[0] & 0xF0;
                uint8_t channel = midi[0] & 0x0F;
                uint8_t data1 = atom->size > 1 ? midi[1] : 0;
                uint8_t data2 = atom->size > 2 ? midi[2] : 0;

                switch (status) {
                    case 0x80: // Note Off
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_note_off(
                            0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                        break;
                    case 0x90: // Note On
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_note_on(
                            0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                        break;
                    case 0xA0: // Poly Pressure
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_paf(
                            0, channel, data1, static_cast<uint32_t>(data2) << 25);
                        break;
                    case 0xB0: // Control Change
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_cc(
                            0, channel, data1, static_cast<uint32_t>(data2) << 25);
                        break;
                    case 0xC0: // Program Change
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_program(
                            0, channel, 0, data1, 0, 0);
                        break;
                    case 0xD0: // Channel Pressure
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_caf(
                            0, channel, static_cast<uint32_t>(data1) << 25);
                        break;
                    case 0xE0: { // Pitch Bend
                        uint32_t value = (static_cast<uint32_t>(data2) << 7) | data1;
                        umpBuffer[umpPosition++] = cmidi2_ump_midi2_pitch_bend_direct(
                            0, channel, value << 18);
                        break;
                    }
                }
            }
        }
    }

    // Update eventOut position
    eventOut.position(umpPosition * sizeof(uint64_t));

    // Log output events for debugging
    if (umpPosition > 0) {
        formatImpl->getLogger()->logInfo("LV2 output events: %zu UMP messages", umpPosition);
        for (size_t i = 0; i < umpPosition; ++i) {
            uint32_t* ump32 = reinterpret_cast<uint32_t*>(&umpBuffer[i]);
            formatImpl->getLogger()->logInfo("  UMP[%zu]: %08X %08X", i, ump32[0], ump32[1]);
        }
    }

    return StatusCode::OK;
}

remidy::PluginParameterSupport *remidy::PluginInstanceLV2::parameters() {
    if (!_parameters)
        _parameters = new ParameterSupport(this);
    return _parameters;
}

remidy::PluginStateSupport *remidy::PluginInstanceLV2::states() {
    if (!_states)
        _states = new PluginStatesLV2(this);
    return _states;
}

remidy::PluginPresetsSupport *remidy::PluginInstanceLV2::presets() {
    if (!_presets)
        _presets = new PresetsSupport(this);
    return _presets;
}

remidy::PluginUISupport *remidy::PluginInstanceLV2::ui() {
    if (!_ui)
        _ui = new UISupport(this);
    return _ui;
}
