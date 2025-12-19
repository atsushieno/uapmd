#include "PluginFormatLV2.hpp"
#include "cmidi2.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <lv2/atom/util.h>

remidy::PluginInstanceLV2::PluginInstanceLV2(PluginCatalogEntry* entry, PluginFormatLV2Impl* formatImpl, const LilvPlugin* plugin) :
        PluginInstance(entry), formatImpl(formatImpl), plugin(plugin),
        implContext(formatImpl->worldContext, formatImpl->world, plugin),
        audio_buses(new AudioBuses(this)) {
}

remidy::PluginInstanceLV2::~PluginInstanceLV2() {
    processing_requested_.store(false, std::memory_order_release);
    if (instance) {
        if (processing_active_) {
            lilv_instance_deactivate(instance);
            processing_active_ = false;
        }
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
    control_atom_port_index = -1;

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
        if (lv2Port.atom_in_index >= 0) {
            auto designationNode = lilv_port_get(plugin, port, implContext.statics->designation_uri_node);
            if (designationNode && implContext.statics->control_designation_uri_node &&
                lilv_node_equals(designationNode, implContext.statics->control_designation_uri_node))
                control_atom_port_index = i;
        }

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
    processing_requested_.store(true, std::memory_order_release);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceLV2::stopProcessing() {
    if (!instance)
        return StatusCode::ALREADY_INVALID_STATE;
    processing_requested_.store(false, std::memory_order_release);
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceLV2::process(AudioProcessContext &process) {
    if (!instance)
        return StatusCode::ALREADY_INVALID_STATE;

    auto shouldProcess = processing_requested_.load(std::memory_order_acquire);
    if (shouldProcess && !processing_active_) {
        lilv_instance_activate(instance);
        processing_active_ = true;
    } else if (!shouldProcess && processing_active_) {
        lilv_instance_deactivate(instance);
        processing_active_ = false;
    }

    if (!shouldProcess)
        return StatusCode::OK;

    // FIXME: is there 64-bit float audio support?
    in_audio_process.store(true, std::memory_order_release);

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

    for (auto & port : lv2_ports) {
        if (port.atom_in_index >= 0 || port.atom_out_index >= 0) {
            lv2_atom_forge_init(&port.forge, getLV2UridMapData());
            lv2_atom_forge_set_buffer(&port.forge, (uint8_t*) port.port_buffer, port.buffer_size);

            if (port.atom_in_index >= 0) {
                // For input ports, clear to empty sequence
                auto* seq = (LV2_Atom_Sequence*) port.port_buffer;
                lv2_atom_sequence_clear(seq);
            } else if (port.atom_out_index >= 0) {
                // For output ports, initialize with buffer capacity in atom.size (in case it is overwritten by the plugin)
                auto* seq = (LV2_Atom_Sequence*) port.port_buffer;
                seq->atom.size = port.buffer_size;
                seq->atom.type = implContext.statics->urids.urid_atom_sequence_type;
                seq->body.unit = implContext.statics->urids.urid_time_frame;
            }
        }
    }

    // FIXME: pass correct timestamp
    ump_input_dispatcher.process(0, process);

    lilv_instance_run(instance, process.frameCount());

    // Deliver any LV2 worker responses generated during this cycle
    remidy_lv2::jalv_worker_emit_responses(&implContext.worker, instance);
    if (implContext.safe_restore) {
        remidy_lv2::jalv_worker_emit_responses(&implContext.state_worker, instance);
    }

    // Process Atom outputs and convert to UMP
    auto& eventOut = process.eventOut();
    auto* umpBuffer = static_cast<uint32_t*>(eventOut.getMessages());
    size_t umpPosition = eventOut.position() / sizeof(uint32_t);
    size_t umpCapacity = eventOut.maxMessagesInBytes() / sizeof(uint32_t);

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

            if (atom->type == implContext.statics->urids.urid_atom_object) {
                const auto* object = reinterpret_cast<const LV2_Atom_Object*>(atom);
                if (object->body.otype == implContext.statics->urids.urid_patch_set) {
                    const LV2_Atom* propertyAtom = nullptr;
                    const LV2_Atom* valueAtom = nullptr;
                    lv2_atom_object_get(object,
                        implContext.statics->urids.urid_patch_property, &propertyAtom,
                        implContext.statics->urids.urid_patch_value, &valueAtom,
                        0);
                    if (!propertyAtom || !valueAtom || propertyAtom->type != implContext.statics->urids.urid_atom_urid_type)
                        continue;
                    auto propertyUrid = reinterpret_cast<const LV2_Atom_URID*>(propertyAtom)->body;
                    double plainValue = 0.0;
                    if (valueAtom->type == implContext.statics->urids.urid_atom_float_type)
                        plainValue = reinterpret_cast<const LV2_Atom_Float*>(valueAtom)->body;
                    else if (valueAtom->type == implContext.statics->urids.urid_atom_int_type)
                        plainValue = static_cast<double>(reinterpret_cast<const LV2_Atom_Int*>(valueAtom)->body);
                    else if (valueAtom->type == implContext.statics->urids.urid_atom_bool_type)
                        plainValue = reinterpret_cast<const LV2_Atom_Bool*>(valueAtom)->body ? 1.0 : 0.0;
                    else
                        continue;
                    auto* lv2Parameters = dynamic_cast<PluginInstanceLV2::ParameterSupport*>(parameters());
                    if (lv2Parameters) {
                        if (auto index = lv2Parameters->indexForProperty(propertyUrid); index.has_value()) {
                            lv2Parameters->notifyParameterValue(index.value(), plainValue);

                            // Notify UI of parameter change
                            auto* lv2UI = dynamic_cast<PluginInstanceLV2::UISupport*>(ui());
                            if (lv2UI)
                                lv2UI->notifyParameterChange(propertyUrid, plainValue);

                            const auto& params = lv2Parameters->parameters();
                            if (index.value() < params.size()) {
                                auto* param = params[index.value()];
                                double range = param->maxPlainValue() - param->minPlainValue();
                                double normalized = range != 0.0
                                    ? (plainValue - param->minPlainValue()) / range
                                    : 0.0;
                                normalized = std::clamp(normalized, 0.0, 1.0);
                                uint8_t bank = static_cast<uint8_t>((index.value() >> 7) & 0x7F);
                                uint8_t idx = static_cast<uint8_t>(index.value() & 0x7F);
                                uint32_t data = static_cast<uint32_t>(normalized * 4294967295.0);
                                uint64_t ump = cmidi2_ump_midi2_nrpn(0, 0, bank, idx, data);
                                if (umpPosition + 1 < umpCapacity) {
                                    umpBuffer[umpPosition++] = static_cast<uint32_t>(ump >> 32);
                                    umpBuffer[umpPosition++] = static_cast<uint32_t>(ump & 0xFFFFFFFF);
                                }
                            }
                        }
                    }
                }
                continue;
            }

            // Check if this is a MIDI event
            if (atom->type == implContext.statics->urids.urid_midi_event_type) {
                // MIDI1 event - convert to MIDI2 UMP
                const uint8_t* midi = (const uint8_t*)(atom + 1);
                uint8_t status = midi[0] & 0xF0;
                uint8_t channel = midi[0] & 0x0F;
                uint8_t data1 = atom->size > 1 ? midi[1] : 0;
                uint8_t data2 = atom->size > 2 ? midi[2] : 0;

                switch (status) {
                    case 0x80: { // Note Off
                        uint64_t ump = cmidi2_ump_midi2_note_off(
                            0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0x90: { // Note On
                        uint64_t ump = cmidi2_ump_midi2_note_on(
                            0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0xA0: { // Poly Pressure
                        uint64_t ump = cmidi2_ump_midi2_paf(
                            0, channel, data1, static_cast<uint32_t>(data2) << 25);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0xB0: { // Control Change
                        uint64_t ump = cmidi2_ump_midi2_cc(
                            0, channel, data1, static_cast<uint32_t>(data2) << 25);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0xC0: { // Program Change
                        uint64_t ump = cmidi2_ump_midi2_program(
                            0, channel, 0, data1, 0, 0);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0xD0: { // Channel Pressure
                        uint64_t ump = cmidi2_ump_midi2_caf(
                            0, channel, static_cast<uint32_t>(data1) << 25);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                    case 0xE0: { // Pitch Bend
                        uint32_t value = (static_cast<uint32_t>(data2) << 7) | data1;
                        uint64_t ump = cmidi2_ump_midi2_pitch_bend_direct(
                            0, channel, value << 18);
                        umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                        umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                        break;
                    }
                }
            }
        }
    }

    // Update eventOut position
    eventOut.position(umpPosition * sizeof(uint32_t));

    in_audio_process.store(false, std::memory_order_release);
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

void remidy::PluginInstanceLV2::enqueueParameterChange(uint32_t index, double value, remidy_timestamp_t timestamp) {
    if (in_audio_process.load(std::memory_order_acquire)) {
        ump_input_dispatcher.enqueuePatchSetEvent(static_cast<int32_t>(index), value, timestamp);
    } else {
        pending_parameter_changes.enqueue(PendingParameterChange{index, value, timestamp});
    }
}

void remidy::PluginInstanceLV2::flushPendingParameterChanges() {
    PendingParameterChange change{};
    while (pending_parameter_changes.try_dequeue(change))
        ump_input_dispatcher.enqueuePatchSetEvent(static_cast<int32_t>(change.index), change.value, change.timestamp);
}

void remidy::PluginInstanceLV2::enqueueAtomEvent(uint32_t port_index, uint32_t buffer_size, uint32_t port_protocol, const void* buffer) {
    PendingAtomEvent event;
    event.port_index = port_index;
    event.buffer_size = buffer_size;
    event.port_protocol = port_protocol;
    event.buffer.resize(buffer_size);
    std::memcpy(event.buffer.data(), buffer, buffer_size);
    pending_atom_events.enqueue(std::move(event));
}

void remidy::PluginInstanceLV2::flushPendingAtomEvents() {
    PendingAtomEvent event;
    while (pending_atom_events.try_dequeue(event)) {
        // Prefer the exact port the UI wrote to if it's a valid atom input
        int32_t targetPortIndex = static_cast<int32_t>(event.port_index);
        if (targetPortIndex < 0 || targetPortIndex >= static_cast<int32_t>(lv2_ports.size()) ||
            lv2_ports[targetPortIndex].atom_in_index < 0) {
            // Fallback: control atom port, else first atom input
            targetPortIndex = control_atom_port_index;
            if (targetPortIndex < 0) {
                for (size_t i = 0; i < lv2_ports.size(); i++) {
                    if (lv2_ports[i].atom_in_index >= 0) {
                        targetPortIndex = static_cast<int32_t>(i);
                        break;
                    }
                }
            }
        }

        if (targetPortIndex < 0 || targetPortIndex >= static_cast<int32_t>(lv2_ports.size())) {
            Logger::global()->logWarning("LV2: Cannot flush atom event - no suitable atom input port found");
            continue;
        }

        auto& port = lv2_ports[targetPortIndex];
        auto& forge = port.forge;

        // Write the atom event into the forge as a sequence event
        const LV2_Atom* atom = reinterpret_cast<const LV2_Atom*>(event.buffer.data());

        // Add frame time (0 for immediate)
        lv2_atom_forge_frame_time(&forge, 0);

        // Recreate the atom header and write the body
        lv2_atom_forge_atom(&forge, atom->size, atom->type);
        lv2_atom_forge_write(&forge, LV2_ATOM_BODY_CONST(atom), atom->size);
    }
}
