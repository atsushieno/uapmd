#include "PluginFormatCLAP.hpp"
#include <umppi/umppi.hpp>
#undef min
#undef max
#include <algorithm>

namespace remidy {
    PluginInstanceCLAP::PluginInstanceCLAP(
        PluginFormatCLAPImpl* owner,
        PluginCatalogEntry* info,
        clap_preset_discovery_factory* presetDiscoveryFactory,
        void* module,
        std::unique_ptr<CLAPPluginProxy> plugin,
        std::unique_ptr<RemidyCLAPHost> host
    ) : PluginInstance(info), owner(owner), preset_discovery_factory(presetDiscoveryFactory), module(module), host(std::move(host)), plugin(std::move(plugin)) {
        if (this->host)
            this->host->attachInstance(this);

        audio_buses = new AudioBuses(this);
        events_in = std::make_unique<clap::helpers::EventList>();
        events_out = std::make_unique<clap::helpers::EventList>();
    }

    PluginInstanceCLAP::~PluginInstanceCLAP() {
        // Destroy UI first to ensure GUI timers are stopped before plugin destruction
        if (_ui) {
            EventLoop::runTaskOnMainThread([&] {
                _ui->destroy();
            });
            delete _ui;
        }

        is_processing.store(false, std::memory_order_release);
        // Stop processing first (audio thread is expected to honour the request before buffers change)
        cleanupBuffers(); // cleanup, optionally stop processing in prior.

        // Deactivate and destroy via proxy (handles thread safety and state checking)
        EventLoop::runTaskOnMainThread([&] {
            if (plugin) {
                plugin->deactivate();
                plugin->destroy();
            }
        });

        if (host)
            host->detachInstance(this);

        delete _parameters;
        delete _states;
        delete _presets;
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

    void applyAudioBuffersToClapProcess(bool isOutput, const clap_audio_buffer_t* dst, GenericAudioBuses* buses, std::vector<clap_audio_buffer_t>& buffers, bool useDouble) {
        auto& bl = isOutput ? buses->audioOutputBuses() : buses->audioInputBuses();
        for (size_t i = 0, n = bl.size(); i < n; i++) {
            if (!bl[i]->enabled())
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
        if (processing_active_)
            owner->getLogger()->logWarning("%s: cleanupBuffers() called while processing is active", info()->displayName().c_str());
        clap_process = clap_process_t();
        resizeAudioPortBuffers(0, false);
        resizeAudioPortBuffers(0, true);
    }

    StatusCode PluginInstanceCLAP::configure(ConfigurationRequest &configuration) {
        bool useDouble = configuration.dataType == AudioContentType::Float64;
        is_offline_ = configuration.offlineMode;
        sample_rate_ = configuration.sampleRate;

        // Check if any port requires 64-bit processing
        bool pluginRequires64Bit = false;
        bool pluginPrefers64Bit = false;
        for (const auto& portInfo : inputPortInfos) {
            if (portInfo.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE) {
                if (portInfo.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) {
                    pluginPrefers64Bit = true;
                    if (portInfo.flags & CLAP_AUDIO_PORT_PREFERS_64BITS)
                        pluginRequires64Bit = true;
                }
            }
        }
        for (const auto& portInfo : outputPortInfos) {
            if (portInfo.flags & CLAP_AUDIO_PORT_REQUIRES_COMMON_SAMPLE_SIZE) {
                if (portInfo.flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS) {
                    pluginPrefers64Bit = true;
                    if (portInfo.flags & CLAP_AUDIO_PORT_PREFERS_64BITS)
                        pluginRequires64Bit = true;
                }
            }
        }

        // If plugin requires 64-bit but host requested 32-bit, log warning and use 64-bit
        if (pluginRequires64Bit && !useDouble) {
            owner->getLogger()->logWarning("%s: Plugin requires 64-bit processing, overriding host configuration",
                                          info()->displayName().c_str());
            useDouble = true;
        }
        // If plugin prefers 64-bit and host supports it, use 64-bit
        else if (pluginPrefers64Bit && configuration.dataType == AudioContentType::Float64) {
            useDouble = true;
        }

        // ensure to clean up old buffer. Note that buses in old configuration may be different,
        // so handle cleanup and allocation in different steps.
        cleanupBuffers();

        // setup audio buses
        audio_buses->configure(configuration);

        // FIXME: provide size via config
        // FIXME: there may be more than one event ports
        transports_events.resize(0x1000);

        // It seems we have to activate plugin buses first.
        EventLoop::runTaskOnMainThread([&] {
            plugin->activate(configuration.sampleRate, 1, configuration.bufferSizeInSamples);
        });
        applyOfflineRenderingMode();

        requires_replacing_process_ = false;
        for (const auto& outInfo : outputPortInfos) {
            if (outInfo.in_place_pair != CLAP_INVALID_ID) {
                requires_replacing_process_ = true;
                break;
            }
        }

        // alter the input/output audio buffers entries, and start allocation.
        clap_process.audio_inputs_count = audio_buses->audioInputBuses().size();
        clap_process.audio_outputs_count = audio_buses->audioOutputBuses().size();
        audio_in_port_buffers.resize(clap_process.audio_inputs_count);
        audio_out_port_buffers.resize(clap_process.audio_outputs_count);

        for (size_t i = 0, n = clap_process.audio_inputs_count; i < n; i++)
            audio_in_port_buffers[i].channel_count = audio_buses->audioInputBuses()[i]->channelLayout().channels();
        for (size_t i = 0, n = clap_process.audio_outputs_count; i < n; i++)
            audio_out_port_buffers[i].channel_count = audio_buses->audioOutputBuses()[i]->channelLayout().channels();

        // Allocate input buffers
        resizeAudioPortBuffers(configuration.bufferSizeInSamples, useDouble);

        // After this, we fix (cannot resize) those audio port buffers.
        clap_process.audio_inputs = audio_in_port_buffers.data();
        clap_process.audio_outputs = audio_out_port_buffers.data();
        applyAudioBuffersToClapProcess(false, clap_process.audio_inputs, audio_buses, audio_in_port_buffers, useDouble);
        applyAudioBuffersToClapProcess(true, clap_process.audio_outputs, audio_buses, audio_out_port_buffers, useDouble);
        clap_process.transport = transports_events.data();

        return StatusCode::OK;
    }

    void PluginInstanceCLAP::applyOfflineRenderingMode() {
        EventLoop::runTaskOnMainThread([&] {
            if (!plugin->canUseRender()) {
                if (is_offline_)
                    owner->getLogger()->logWarning("%s: offlineMode requested but plugin lacks CLAP_EXT_RENDER",
                                                   info()->displayName().c_str());
                return;
            }

            if (is_offline_ && plugin->renderHasHardRealtimeRequirement()) {
                owner->getLogger()->logWarning("%s: plugin requires realtime rendering, cannot switch to offline",
                                               info()->displayName().c_str());
                return;
            }

            auto mode = is_offline_ ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME;
            if (!plugin->renderSet(mode)) {
                owner->getLogger()->logWarning("%s: failed to set render mode to %s", info()->displayName().c_str(),
                                               is_offline_ ? "offline" : "realtime");
            }
        });
    }


    StatusCode PluginInstanceCLAP::startProcessing() {
        is_processing.store(true, std::memory_order_release);
        return StatusCode::OK;
    }

    StatusCode PluginInstanceCLAP::stopProcessing() {
        is_processing.store(false, std::memory_order_release);
        return StatusCode::OK;
    }

    void PluginInstanceCLAP::remidyProcessContextToClapProcess(clap_process_t& dst, AudioProcessContext& src) {

        dst.frames_count = src.frameCount();
        // FIXME: should we calculate something based on DCTPQ i.e. src.trackContext()->deltaClockstampTicksPerQuarterNotes() ?
        dst.steady_time = -1;

        // Update transport information from MasterContext
        auto* trackContext = src.trackContext();
        auto& masterContext = trackContext->masterContext();

        if (!transports_events.empty()) {
            auto& transport = transports_events[0];
            transport.header.size = sizeof(clap_event_transport_t);
            transport.header.time = 0;
            transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            transport.header.type = CLAP_EVENT_TRANSPORT;
            transport.header.flags = 0;

            // Set transport state flags
            transport.flags = 0;
            if (masterContext.isPlaying()) {
                transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
            }
            transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
            transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;

            // Set position in samples
            transport.song_pos_seconds = static_cast<double>(masterContext.playbackPositionSamples()) / masterContext.sampleRate();
            transport.song_pos_beats = -1; // Will be calculated from tempo

            // tempo in masterContext is in microseconds per quarter note, convert to BPM
            double tempoBPM = 60000000.0 / masterContext.tempo();
            transport.tempo = tempoBPM;

            // Calculate beat position
            double seconds = static_cast<double>(masterContext.playbackPositionSamples()) / masterContext.sampleRate();
            transport.song_pos_beats = (seconds * tempoBPM) / 60.0;

            // Time signature (default 4/4)
            transport.tsig_num = 4;
            transport.tsig_denom = 4;
        }

        const bool useDouble = src.trackContext()->masterContext().audioDataType() == AudioContentType::Float64;
        const size_t numFrames = static_cast<size_t>(src.frameCount());

        auto zeroInputFallback = [&](size_t bus, size_t channel) -> std::pair<float*, double*> {
            float* f32 = nullptr;
            double* f64 = nullptr;
            if (bus < audio_in_port_buffers.size()) {
                auto& buffer = audio_in_port_buffers[bus];
                if (channel < buffer.channel_count) {
                    if (buffer.data32 && buffer.data32[channel]) {
                        std::fill_n(buffer.data32[channel], numFrames, 0.0f);
                        f32 = buffer.data32[channel];
                    }
                    if (buffer.data64 && buffer.data64[channel]) {
                        std::fill_n(buffer.data64[channel], numFrames, 0.0);
                        f64 = buffer.data64[channel];
                    }
                }
            }
            return {f32, f64};
        };

        auto prepareOutputFallback = [&](size_t bus, size_t channel) -> std::pair<float*, double*> {
            float* f32 = nullptr;
            double* f64 = nullptr;
            if (bus < audio_out_port_buffers.size()) {
                auto& buffer = audio_out_port_buffers[bus];
                if (channel < buffer.channel_count) {
                    if (buffer.data32 && buffer.data32[channel])
                        f32 = buffer.data32[channel];
                    if (buffer.data64 && buffer.data64[channel])
                        f64 = buffer.data64[channel];
                }
            }
            return {f32, f64};
        };

        const int32_t hostInputBuses = src.audioInBusCount();
        for (size_t bus = 0; bus < dst.audio_inputs_count; ++bus) {
            auto& audioIn = dst.audio_inputs[bus];
            const bool hostHasBus = static_cast<int32_t>(bus) < hostInputBuses;
            const int32_t hostChannels = hostHasBus ? src.inputChannelCount(static_cast<int32_t>(bus)) : 0;
            for (size_t ch = 0; ch < audioIn.channel_count; ++ch) {
                if (!useDouble) {
                    float* ptr = nullptr;
                    if (hostHasBus && ch < static_cast<size_t>(hostChannels))
                        ptr = src.getFloatInBuffer(static_cast<int32_t>(bus), static_cast<uint32_t>(ch));
                    else if (hostHasBus && hostChannels > 0)
                        ptr = src.getFloatInBuffer(static_cast<int32_t>(bus), 0);
                    if (!ptr)
                        ptr = zeroInputFallback(bus, ch).first;
                    audioIn.data32[ch] = ptr;
                } else {
                    double* ptr = nullptr;
                    if (hostHasBus && ch < static_cast<size_t>(hostChannels))
                        ptr = src.getDoubleInBuffer(static_cast<int32_t>(bus), static_cast<uint32_t>(ch));
                    else if (hostHasBus && hostChannels > 0)
                        ptr = src.getDoubleInBuffer(static_cast<int32_t>(bus), 0);
                    if (!ptr)
                        ptr = zeroInputFallback(bus, ch).second;
                    audioIn.data64[ch] = ptr;
                }
            }
        }

        const int32_t hostOutputBuses = src.audioOutBusCount();
        for (size_t bus = 0; bus < dst.audio_outputs_count; ++bus) {
            auto& audioOut = dst.audio_outputs[bus];
            const bool hostHasBus = static_cast<int32_t>(bus) < hostOutputBuses;
            const int32_t hostChannels = hostHasBus ? src.outputChannelCount(static_cast<int32_t>(bus)) : 0;
            for (size_t ch = 0; ch < audioOut.channel_count; ++ch) {
                if (!useDouble) {
                    float* ptr = nullptr;
                    if (hostHasBus && ch < static_cast<size_t>(hostChannels))
                        ptr = src.getFloatOutBuffer(static_cast<int32_t>(bus), static_cast<uint32_t>(ch));
                    if (!ptr)
                        ptr = prepareOutputFallback(bus, ch).first;
                    audioOut.data32[ch] = ptr;
                } else {
                    double* ptr = nullptr;
                    if (hostHasBus && ch < static_cast<size_t>(hostChannels))
                        ptr = src.getDoubleOutBuffer(static_cast<int32_t>(bus), static_cast<uint32_t>(ch));
                    if (!ptr)
                        ptr = prepareOutputFallback(bus, ch).second;
                    audioOut.data64[ch] = ptr;
                }
            }
        }

        // set event buffers
        clap_process.in_events = events_in->clapInputEvents();
        clap_process.out_events = events_out->clapOutputEvents();
    }

    StatusCode PluginInstanceCLAP::process(AudioProcessContext &process) {
        // CLAP requires parameter flush callbacks on the audio thread even when no audio is running
        if (flush_requested_.load(std::memory_order_acquire)) {
            flush_requested_.store(false, std::memory_order_release);
            processParamsFlush();
        }

        auto shouldProcess = is_processing.load(std::memory_order_acquire);
        if (!shouldProcess) {
            if (processing_active_) {
                plugin->stopProcessing();
                processing_active_ = false;
            }
            return StatusCode::OK;
        }
        if (!processing_active_) {
            if (!plugin->startProcessing()) {
                owner->getLogger()->logError("%s: clap_plugin.start_processing() failed on audio thread",
                                             info()->displayName().c_str());
                is_processing.store(false, std::memory_order_release);
                return StatusCode::FAILED_TO_START_PROCESSING;
            }
            processing_active_ = true;
        }

        // fill clap_process from remidy input
        remidyProcessContextToClapProcess(clap_process, process);

        // Prepare output event list for this audio block
        events_out->clear();

        ump_input_dispatcher.process(process);

        // FIXME: we should report process result somehow
        auto result = plugin->process(&clap_process);
        auto ret = result == CLAP_PROCESS_ERROR ? StatusCode::FAILED_TO_PROCESS : StatusCode::OK;

        // Convert CLAP output events to UMP
        auto& eventOut = process.eventOut();
        auto* umpBuffer = static_cast<uint32_t*>(eventOut.getMessages());
        size_t umpPosition = eventOut.position() / sizeof(uint32_t); // position in uint32_t units
        size_t umpCapacity = eventOut.maxMessagesInBytes() / sizeof(uint32_t);

        // Process CLAP output events
        size_t eventCount = events_out->size();
        for (size_t i = 0; i < eventCount && umpPosition < umpCapacity; ++i) {
            auto* hdr = events_out->get(static_cast<uint32_t>(i));

            if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;

            switch (hdr->type) {
                case CLAP_EVENT_NOTE_ON: {
                    auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                    uint8_t group = 0; // CLAP doesn't have groups
                    uint8_t channel = ev->channel >= 0 ? static_cast<uint8_t>(ev->channel) : 0;
                    uint8_t note = ev->key >= 0 ? static_cast<uint8_t>(ev->key) : 0;
                    uint16_t velocity = static_cast<uint16_t>(ev->velocity * UINT16_MAX);
                    uint64_t ump = umppi::UmpFactory::midi2NoteOn(group, channel, note, 0, velocity, 0);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case CLAP_EVENT_NOTE_OFF: {
                    auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                    uint8_t group = 0;
                    uint8_t channel = ev->channel >= 0 ? static_cast<uint8_t>(ev->channel) : 0;
                    uint8_t note = ev->key >= 0 ? static_cast<uint8_t>(ev->key) : 0;
                    uint16_t velocity = static_cast<uint16_t>(ev->velocity * UINT16_MAX);
                    uint64_t ump = umppi::UmpFactory::midi2NoteOff(group, channel, note, 0, velocity, 0);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case CLAP_EVENT_PARAM_VALUE: {
                    auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                    if (_parameters) {
                        auto* params = dynamic_cast<ParameterSupport*>(_parameters);
                        if (params) {
                            // LAMESPEC:
                            // clap-juce-extensions sets note_id = 0 even if the parameter is NOT a per-note controller.
                            // Then it results in the wrong code path below without this extraneous check for the parameter flags.
                            // It is allowed to set 0 as a note_id when the parameter is NOT a per-note controller?
                            // There is NO official definition that a CLAP parameter is per-note or not.
                            // The only way to distinguish them is to use CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID
                            // (but there can be non-automatable parameters which works per note ID).
                            //
                            // CLAP specification should clearly declare that non-per-note parameter changes MUST specify
                            // note_id to be always -1. Those clap_juce_extension developers don't think it is a MUST.
                            bool isPNC = params->clapParameterFlags(ev->param_id) & CLAP_PARAM_IS_AUTOMATABLE_PER_NOTE_ID;
                            if (isPNC && ev->note_id >= 0) {
                                // Polyphonic modulation - check which dimension is being used
                                if (ev->key >= 0) {
                                    params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_NOTE,
                                                                          static_cast<uint32_t>(ev->key),
                                                                          ev->param_id,
                                                                          ev->value);
                                } else if (ev->channel >= 0) {
                                    params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_CHANNEL,
                                                                          static_cast<uint32_t>(ev->channel),
                                                                          ev->param_id,
                                                                          ev->value);
                                } else if (ev->port_index >= 0) {
                                    params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_GROUP,
                                                                          static_cast<uint32_t>(ev->port_index),
                                                                          ev->param_id,
                                                                          ev->value);
                                }
                            } else {
                                // Regular parameter change (note_id == -1)
                                params->notifyParameterValue(ev->param_id, ev->value);
                            }
                        }
                    }
                }
                case CLAP_EVENT_MIDI: {
                    // Convert MIDI1 to MIDI2 UMP
                    auto* ev = reinterpret_cast<const clap_event_midi_t*>(hdr);
                    uint8_t status = ev->data[0] & 0xF0;
                    uint8_t channel = ev->data[0] & 0x0F;
                    uint8_t data1 = ev->data[1];
                    uint8_t data2 = ev->data[2];

                    switch (status) {
                        case 0x80: { // Note Off
                            uint64_t ump = umppi::UmpFactory::midi2NoteOff(
                                0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0x90: { // Note On
                            uint64_t ump = umppi::UmpFactory::midi2NoteOn(
                                0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0xA0: { // Poly Pressure
                            uint64_t ump = umppi::UmpFactory::midi2PAf(
                                0, channel, data1, static_cast<uint32_t>(data2) << 25);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0xB0: { // Control Change
                            uint64_t ump = umppi::UmpFactory::midi2CC(
                                0, channel, data1, static_cast<uint32_t>(data2) << 25);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0xC0: { // Program Change
                            uint64_t ump = umppi::UmpFactory::midi2Program(
                                0, channel, 0, data1, 0, 0);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0xD0: { // Channel Pressure
                            uint64_t ump = umppi::UmpFactory::midi2CAf(
                                0, channel, static_cast<uint32_t>(data1) << 25);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                        case 0xE0: { // Pitch Bend
                            uint32_t value = (static_cast<uint32_t>(data2) << 7) | data1;
                            uint64_t ump = umppi::UmpFactory::midi2PitchBendDirect(
                                0, channel, value << 18);
                            umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                            umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                            break;
                        }
                    }
                    break;
                }
                case CLAP_EVENT_MIDI2: {
                    // MIDI2 UMP - copy directly
                    auto* ev = reinterpret_cast<const clap_event_midi2_t*>(hdr);
                    // Check UMP size and copy
                    uint8_t messageType = (ev->data[0] >> 28) & 0xF;
                    if (messageType <= 3) {
                        // 32-bit message (1 uint32_t)
                        if (umpPosition < umpCapacity) {
                            umpBuffer[umpPosition++] = ev->data[0];
                        }
                    } else if (messageType == 4 || messageType == 5) {
                        // 64-bit message (2 uint32_t)
                        if (umpPosition + 1 < umpCapacity) {
                            umpBuffer[umpPosition++] = ev->data[0];
                            umpBuffer[umpPosition++] = ev->data[1];
                        }
                    } else if (messageType >= 0xD) {
                        // 128-bit message (4 uint32_t)
                        if (umpPosition + 3 < umpCapacity) {
                            umpBuffer[umpPosition++] = ev->data[0];
                            umpBuffer[umpPosition++] = ev->data[1];
                            umpBuffer[umpPosition++] = ev->data[2];
                            umpBuffer[umpPosition++] = ev->data[3];
                        }
                    }
                    break;
                }
                default:
                    // Other event types not yet supported
                    break;
            }
        }

        // Update eventOut position
        eventOut.position(umpPosition * sizeof(uint32_t));
        events_out->clear();
        events_in->clear();

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

    remidy::PluginPresetsSupport *remidy::PluginInstanceCLAP::presets() {
        if (!_presets)
            _presets = new PresetsSupport(this);
        return _presets;
    }

    PluginUISupport* PluginInstanceCLAP::ui() {
        if (!_ui)
            _ui = new UISupport(this);
        return _ui;
    }

    bool PluginInstanceCLAP::handleGuiResize(uint32_t width, uint32_t height) {
        if (!_ui)
            return false;
        auto* clapUi = dynamic_cast<UISupport*>(_ui);
        if (!clapUi)
            return false;
        return clapUi->handleGuiResize(width, height);
    }

    void PluginInstanceCLAP::dispatchTimer(clap_id timerId) {
        if (!plugin || !plugin->canUseTimerSupport())
            return;
        // Ensure timer callback happens on the main/UI thread per CLAP expectations
        EventLoop::runTaskOnMainThread([this, timerId](){
            plugin->timerSupportOnTimer(timerId);
        });
    }

    void PluginInstanceCLAP::processParamsFlush() {
        if (!plugin || !plugin->canUseParams())
            return;

        // Use dedicated event lists for parameter flush exchanges
        events_out->clear();
        clap::helpers::EventList flushInputEvents;
        plugin->paramsFlush(
                flushInputEvents.clapInputEvents(),
                events_out->clapOutputEvents()
        );

        // Process any output events from the flush
        size_t eventCount = events_out->size();
        for (size_t i = 0; i < eventCount; ++i) {
            auto* hdr = events_out->get(static_cast<uint32_t>(i));

            if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;

            if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
                auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                auto* params = dynamic_cast<ParameterSupport*>(_parameters);
                if (params) {
                    if (ev->key >= 0) {
                        params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_NOTE,
                                                             static_cast<uint32_t>(ev->key),
                                                             ev->param_id,
                                                             ev->value);
                    } else if (ev->channel >= 0) {
                        params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_CHANNEL,
                                                             static_cast<uint32_t>(ev->channel),
                                                             ev->param_id,
                                                             ev->value);
                    } else if (ev->port_index >= 0) {
                        params->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_GROUP,
                                                             static_cast<uint32_t>(ev->port_index),
                                                             ev->param_id,
                                                             ev->value);
                    } else {
                        params->notifyParameterValue(ev->param_id, ev->value);
                    }
                }
            }
        }

        events_out->clear();
    }
}
