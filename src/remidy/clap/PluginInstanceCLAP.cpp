#include "PluginFormatCLAP.hpp"

namespace remidy {
    PluginInstanceCLAP::PluginInstanceCLAP(
        PluginFormatCLAP::Impl* owner,
        PluginCatalogEntry* info,
        void* module,
        const clap_plugin_t* plugin
    ) : PluginInstance(info), owner(owner), plugin(plugin), module(module) {
        plugin->init(plugin);
    }

    PluginInstanceCLAP::~PluginInstanceCLAP() {
        plugin->destroy(plugin);
    }

    StatusCode PluginInstanceCLAP::configure(ConfigurationRequest &configuration) {


        plugin->activate(plugin, configuration.sampleRate, 1, configuration.bufferSizeInSamples);
        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::startProcessing() {
        plugin->start_processing(plugin);
        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::stopProcessing() {
        plugin->stop_processing(plugin);
        return StatusCode::OK;
    }

    void PluginInstanceCLAP::remidyProcessContextToClapProcess(clap_process_t& dst, AudioProcessContext& src) {

        dst.frames_count = src.frameCount();
        // FIXME: should we calculate something based on DCTPQ i.e. src.trackContext()->deltaClockstampTicksPerQuarterNotes() ?
        dst.steady_time = -1;

        // copy audio buffer pointers
        dst.audio_inputs_count = src.audioInBusCount();
        for (size_t bus = 0, nBus = dst.audio_inputs_count; bus < nBus; bus++) {
            if (src.trackContext()->masterContext().audioDataType() == AudioContentType::Float32)
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_inputs[bus].data32[ch] = src.getFloatInBuffer(bus, ch);
            else
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_inputs[bus].data64[ch] = src.getDoubleInBuffer(bus, ch);
        }

        dst.audio_outputs_count = src.audioOutBusCount();
        for (size_t bus = 0, nBus = dst.audio_outputs_count; bus < nBus; bus++) {
            if (src.trackContext()->masterContext().audioDataType() == AudioContentType::Float32)
                for (size_t ch = 0, nCh = src.outputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_outputs[bus].data32[ch] = src.getFloatOutBuffer(bus, ch);
            else
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_outputs[bus].data64[ch] = src.getDoubleOutBuffer(bus, ch);
        }
    }

    StatusCode PluginInstanceCLAP::process(AudioProcessContext &process) {
        // fill clap_process from remidy input
        remidyProcessContextToClapProcess(clap_process, process);

        // FIXME: provide valid timestamp?
        ump_input_dispatcher.process(0, process);

        // FIXME: we should report process result somehow
        auto result = plugin->process(plugin, &clap_process);
        auto ret = result == CLAP_PROCESS_ERROR ? StatusCode::FAILED_TO_PROCESS : StatusCode::OK;

        // FIXME: process eventOut

        return ret;
    }

    PluginParameterSupport* PluginInstanceCLAP::parameters() {
        if (!_parameters)
            _parameters = new ParameterSupport(this);
        return _parameters;
    }
}