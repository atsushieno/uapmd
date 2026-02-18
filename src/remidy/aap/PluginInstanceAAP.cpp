#include "PluginFormatAAP.hpp"

remidy::PluginInstanceAAP::PluginInstanceAAP(
        PluginFormatAAPImpl* format, PluginCatalogEntry* entry, aap::PluginInstance* aapInstance
) : PluginInstance(entry), format(format), instance(aapInstance) {
}

remidy::StatusCode
remidy::PluginInstanceAAP::configure(remidy::PluginInstance::ConfigurationRequest &configuration) {
    // FIXME: not verified at all.

    aapInstance()->prepare(configuration.bufferSizeInSamples);
    // generate port mappings
    remidy_to_aap_port_index_map_audio_in.clear();
    remidy_to_aap_port_index_map_audio_out.clear();

    for (auto i = 0, n = aapInstance()->getNumPorts(); i < n; i++) {
        auto port = aapInstance()->getPort(i);
        if (port->getContentType() == AAP_CONTENT_TYPE_AUDIO) {
            if (port->getPortDirection() & AAP_PORT_DIRECTION_INPUT)
                remidy_to_aap_port_index_map_audio_in.push_back(i);
            if (port->getPortDirection() & AAP_PORT_DIRECTION_OUTPUT)
                remidy_to_aap_port_index_map_audio_out.push_back(i);
        }
        if (port->getContentType() == AAP_CONTENT_TYPE_MIDI2) {
            if (port->getPortDirection() & AAP_PORT_DIRECTION_INPUT)
                aap_port_midi2_in = i;
            if (port->getPortDirection() & AAP_PORT_DIRECTION_OUTPUT)
                aap_port_midi2_out = i;
        }
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::startProcessing() {
    aapInstance()->activate();
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::stopProcessing() {
    aapInstance()->deactivate();
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAAP::process(remidy::AudioProcessContext &process) {
    // FIXME: not verified at all.

    size_t aapIdx;
    auto instance = aapInstance();
    auto buffer = instance->getAudioPluginBuffer();

    aapIdx = 0;
    for (auto iBus = 0, nBus = process.audioInBusCount(); iBus < nBus; iBus++) {
        for (auto iCh = 0, nCh = process.inputChannelCount(iBus); iCh < nCh; iCh++) {
            if (remidy_to_aap_port_index_map_audio_in.size() <= aapIdx)
                break;
            auto aapPortIdx = remidy_to_aap_port_index_map_audio_in[aapIdx++];
            auto src = process.getFloatInBuffer(iBus, iCh);
            auto dst = buffer->get_buffer(*buffer, aapPortIdx);
            memcpy(dst, src, sizeof(float) * process.frameCount());
        }
        // FIXME: we should iterate non-main buses too once AAP is ready for that.
        break;
    }
    if (aap_port_midi2_in >= 0) {
        auto& eIn = process.eventIn();
        auto dst = buffer->get_buffer(*buffer, aap_port_midi2_in);
        memcpy(dst, eIn.getMessages(), eIn.position());
    }

    instance->process(process.frameCount(), 0);

    aapIdx = 0;
    for (auto iBus = 0, nBus = process.audioOutBusCount(); iBus < nBus; iBus++) {
        for (auto iCh = 0, nCh = process.outputChannelCount(iBus); iCh < nCh; iCh++) {
            if (remidy_to_aap_port_index_map_audio_out.size() <= aapIdx)
                break;
            auto aapPortIdx = remidy_to_aap_port_index_map_audio_out[aapIdx++];
            auto dst = process.getFloatInBuffer(iBus, iCh);
            auto src = buffer->get_buffer(*buffer, aapPortIdx);
            memcpy(dst, src, sizeof(float) * process.frameCount());
        }
        // FIXME: we should iterate non-main buses too once AAP is ready for that.
        break;
    }
    // FIXME: we need some API fixes so that we can get reliable event out size
    /*
    if (aap_port_midi2_out >= 0) {
        auto& eIn = process.eventOut();
        auto src = buffer->get_buffer(*buffer, aap_port_midi2_out);
        auto size = buffer->get_buffer_size(*buffer, aap_port_midi2_out);
        memcpy(eIn.getMessages(), src, size);
    }*/

    return StatusCode::OK;
}
