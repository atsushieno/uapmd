#include "PluginFormatCLAP.hpp"

namespace remidy {
    PluginInstanceCLAP::PluginInstanceCLAP(
        PluginFormatCLAP::Impl* owner,
        PluginCatalogEntry* info,
        void* module,
        const clap_plugin_t* plugin
    ) : PluginInstance(info), owner(owner), plugin(plugin), module(module) {
        plugin->init(plugin);

        audio_buses = new AudioBuses(this);
    }

    PluginInstanceCLAP::~PluginInstanceCLAP() {
        cleanupBuffers(); // cleanup, optionally stop processing in prior.

        plugin->destroy(plugin);
    }

    void resetCLAPAudioBuffers(clap_audio_buffer_t& a) {
        for (size_t ch = 0, nCh = a.channel_count; ch < nCh; ch++) {
            if (a.data32[ch])
                free(a.data32[ch]);
        }
        for (size_t ch = 0, nCh = a.channel_count; ch < nCh; ch++) {
            if (a.data64[ch])
                free(a.data64[ch]);
        }
    }

    void resizeCLAPAudioBuffers(clap_audio_buffer_t& a, size_t newSizeInSamples, bool isDouble) {
        if (!isDouble && !a.data32)
            a.data32 = new float*[a.channel_count];
        if (isDouble && !a.data64)
            a.data64 = new double*[a.channel_count];
        if (isDouble) {
            for (size_t ch = 0, nCh = a.channel_count; ch < nCh; ch++)
                a.data64[ch] = static_cast<double *>(calloc(newSizeInSamples, sizeof(double)));
        } else {
            for (size_t ch = 0, nCh = a.channel_count; ch < nCh; ch++)
                a.data32[ch] = static_cast<float *>(calloc(newSizeInSamples, sizeof(float)));
        }
    }

    void PluginInstanceCLAP::resetAudioPortBuffers() {
        for (auto &a : audio_in_port_buffers)
            resetCLAPAudioBuffers(a);
        for (auto &a : audio_out_port_buffers)
            resetCLAPAudioBuffers(a);
    }

    void PluginInstanceCLAP::resizeAudioPortBuffers(size_t newSizeInSamples, bool isDouble) {
        for (auto &a : audio_in_port_buffers)
            resizeCLAPAudioBuffers(a, newSizeInSamples, isDouble);
        for (auto &a : audio_out_port_buffers)
            resizeCLAPAudioBuffers(a, newSizeInSamples, isDouble);
    }

    void applyAudioBuffersToClapProcess(const clap_audio_buffer_t* dst, GenericAudioBuses* buses, std::vector<clap_audio_buffer_t>& buffers, bool useDouble) {
        for (size_t i = 0, n = buses->audioInputBuses().size(); i < n; i++) {
            if (!buses->audioInputBuses()[i]->enabled())
                continue;
            const auto src = buffers[i];
            for (size_t ch = 0, nCh = src.channel_count; ch < nCh; ch++) {
                if (useDouble)
                    dst[i].data64[ch] = src.data64[ch];
                else
                    dst[i].data32[ch] = src.data32[ch];
            }
        }
    }

    void PluginInstanceCLAP::cleanupBuffers() {
        stopProcessing(); // make sure we do not process audio while altering buffers.
        clap_process = clap_process_t();
        resizeAudioPortBuffers(0, false);
        resizeAudioPortBuffers(0, true);
    }

    StatusCode PluginInstanceCLAP::configure(ConfigurationRequest &configuration) {
        bool useDouble = configuration.dataType == AudioContentType::Float64;

        // ensure to clean up old buffer. Note that buses in old configuration may be different,
        // so handle cleanup and allocation in different steps.
        cleanupBuffers();

        // setup audio buses
        audio_buses->configure(configuration);

        // FIXME: provide size via config
        // FIXME: there may be more than one event ports
        transports_events.resize(0x1000);

        // It seems we have to activate plugin buses first.
        plugin->activate(plugin, configuration.sampleRate, 1, configuration.bufferSizeInSamples);

        // alter the input/output audio buffers entries, and start allocation.
        clap_process.audio_inputs_count = audio_buses->audioInputBuses().size();
        clap_process.audio_outputs_count = audio_buses->audioOutputBuses().size();
        audio_in_port_buffers.resize(clap_process.audio_inputs_count);
        audio_out_port_buffers.resize(clap_process.audio_outputs_count);
        for (size_t i = 0, n = clap_process.audio_inputs_count; i < n; i++)
            audio_in_port_buffers[i].channel_count = audio_buses->audioInputBuses()[i]->channelLayout().channels();
        for (size_t i = 0, n = clap_process.audio_outputs_count; i < n; i++)
            audio_out_port_buffers[i].channel_count = audio_buses->audioOutputBuses()[i]->channelLayout().channels();

        resizeAudioPortBuffers(configuration.bufferSizeInSamples, useDouble);

        // After this, we fix (cannot resize) those audio port buffers.
        clap_process.audio_inputs = audio_in_port_buffers.data();
        clap_process.audio_outputs = audio_out_port_buffers.data();
        applyAudioBuffersToClapProcess(clap_process.audio_inputs, audio_buses, audio_in_port_buffers, useDouble);
        applyAudioBuffersToClapProcess(clap_process.audio_outputs, audio_buses, audio_out_port_buffers, useDouble);
        clap_process.transport = transports_events.data();

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
        auto numAudioIn = std::min((int32_t) dst.audio_inputs_count, src.audioInBusCount());
        for (size_t bus = 0, nBus = numAudioIn; bus < nBus; bus++) {
            if (src.trackContext()->masterContext().audioDataType() == AudioContentType::Float32)
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_inputs[bus].data32[ch] = src.getFloatInBuffer(bus, ch);
            else
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_inputs[bus].data64[ch] = src.getDoubleInBuffer(bus, ch);
        }

        auto numAudioOut = std::min((int32_t) dst.audio_outputs_count, src.audioOutBusCount());
        for (size_t bus = 0, nBus = numAudioOut; bus < nBus; bus++) {
            if (src.trackContext()->masterContext().audioDataType() == AudioContentType::Float32)
                for (size_t ch = 0, nCh = src.outputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_outputs[bus].data32[ch] = src.getFloatOutBuffer(bus, ch);
            else
                for (size_t ch = 0, nCh = src.inputChannelCount(bus); ch < nCh; ch++)
                    dst.audio_outputs[bus].data64[ch] = src.getDoubleOutBuffer(bus, ch);
        }

        // set event buffers
        clap_process.in_events = events.clapInputEvents();
        clap_process.out_events = events.clapOutputEvents();
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

        events.clear();

        return ret;
    }

    PluginParameterSupport* PluginInstanceCLAP::parameters() {
        if (!_parameters)
            _parameters = new ParameterSupport(this);
        return _parameters;
    }

    PluginStateSupport *PluginInstanceCLAP::states() {
        if (!_states)
            _states = new PluginStatesCLAP(this);
        return _states;
    }
}
