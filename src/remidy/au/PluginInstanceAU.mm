#if __APPLE__

#include "PluginFormatAU.hpp"
#include "cmidi2.h"
#include <cmath>

remidy::PluginInstanceAUv2::PluginInstanceAUv2(
        PluginFormatAUv3 *format,
        PluginFormat::PluginInstantiationOptions options,
        Logger* logger,
        PluginCatalogEntry* info,
        AudioComponent component,
        AudioComponentInstance instance
) : PluginInstance(info), format(format), options(options), logger_(logger), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
    setCurrentThreadNameIfPossible("remidy.AU.instance." + name);
    audio_buses = new AudioBuses(this);
    initializeHostCallbacks();
}

remidy::PluginInstanceAUv2::~PluginInstanceAUv2() {
    if (options.uiThreadRequirement & PluginUIThreadRequirement::InstanceControl)
        EventLoop::runTaskOnMainThread([&] {
            AudioComponentInstanceDispose(instance);
        });
    else
        AudioComponentInstanceDispose(instance);
    delete audio_buses;
    delete _parameters;
    delete _states;
    delete _presets;

    for (auto auDataIn : auDataIns)
        free(auDataIn);
    for (auto auDataOut : auDataOuts)
        free(auDataOut);
}

void remidy::PluginInstanceAUv2::initializeHostCallbacks() {
    host_transport_info = HostTransportInfo{};
    host_callback_info = HostCallbackInfo{};
    host_callback_info.hostUserData = this;
    host_callback_info.beatAndTempoProc = &PluginInstanceAUv2::hostCallbackGetBeatAndTempo;
    host_callback_info.musicalTimeLocationProc = &PluginInstanceAUv2::hostCallbackGetMusicalTimeLocation;
    host_callback_info.transportStateProc = &PluginInstanceAUv2::hostCallbackGetTransportState;
    host_callback_info.transportStateProc2 = &PluginInstanceAUv2::hostCallbackGetTransportState2;

    auto status = AudioUnitSetProperty(instance,
        kAudioUnitProperty_HostCallbacks,
        kAudioUnitScope_Global,
        0,
        &host_callback_info,
        sizeof(host_callback_info));

    if (status != noErr) {
        logger()->logWarning("%s: failed to set kAudioUnitProperty_HostCallbacks. Status: %d", name.c_str(), status);
    }
}

OSStatus remidy::PluginInstanceAUv2::audioInputRenderCallback(
        AudioUnitRenderActionFlags *ioActionFlags,
        const AudioTimeStamp *inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList *ioData
) {
    auto auDataIn = auDataIns[inBusNumber];
    for (uint32_t ch = 0; ch < auDataIn->mNumberBuffers; ch++)
        ioData->mBuffers[ch] = auDataIn->mBuffers[ch];
    ioData->mNumberBuffers = auDataIn->mNumberBuffers;

    return noErr;
}

OSStatus remidy::PluginInstanceAUv2::midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
    if (!pktlist || pktlist->numPackets == 0)
        return noErr;

    // Ensure buffer has enough capacity (estimate 1 UMP per packet)
    if (midi_output_buffer.size() < midi_output_count + pktlist->numPackets) {
        midi_output_buffer.resize(midi_output_count + pktlist->numPackets + 128);
    }

    const MIDIPacket* packet = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
        if (packet->length > 0 && midi_output_count < midi_output_buffer.size()) {
            const Byte* data = packet->data;
            uint8_t status = data[0] & 0xF0;
            uint8_t channel = data[0] & 0x0F;
            uint8_t data1 = packet->length > 1 ? data[1] : 0;
            uint8_t data2 = packet->length > 2 ? data[2] : 0;

            // Convert MIDI1 to MIDI2 UMP
            uint64_t ump;
            switch (status) {
                case 0x80: // Note Off
                    ump = cmidi2_ump_midi2_note_off(
                        0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                    break;
                case 0x90: // Note On
                    ump = cmidi2_ump_midi2_note_on(
                        0, channel, data1, 0, static_cast<uint16_t>(data2) << 9, 0);
                    break;
                case 0xA0: // Poly Pressure
                    ump = cmidi2_ump_midi2_paf(
                        0, channel, data1, static_cast<uint32_t>(data2) << 25);
                    break;
                case 0xB0: // Control Change
                    ump = cmidi2_ump_midi2_cc(
                        0, channel, data1, static_cast<uint32_t>(data2) << 25);
                    break;
                case 0xC0: // Program Change
                    ump = cmidi2_ump_midi2_program(
                        0, channel, 0, data1, 0, 0);
                    break;
                case 0xD0: // Channel Pressure
                    ump = cmidi2_ump_midi2_caf(
                        0, channel, static_cast<uint32_t>(data1) << 25);
                    break;
                case 0xE0: { // Pitch Bend
                    uint32_t value = (static_cast<uint32_t>(data2) << 7) | data1;
                    ump = cmidi2_ump_midi2_pitch_bend_direct(
                        0, channel, value << 18);
                    break;
                }
                default:
                    continue; // Skip unknown messages
            }
            // Write as two uint32_t words
            if (midi_output_count + 1 < midi_output_buffer.size()) {
                midi_output_buffer[midi_output_count++] = (uint32_t)(ump >> 32);
                midi_output_buffer[midi_output_count++] = (uint32_t)(ump & 0xFFFFFFFF);
            }
        }
        packet = MIDIPacketNext(packet);
    }

    return noErr;
}

remidy::StatusCode remidy::PluginInstanceAUv2::configure(ConfigurationRequest& configuration) {
    OSStatus result;
    UInt32 size; // unused field for AudioUnitGetProperty

    result = AudioUnitReset(instance, kAudioUnitScope_Global, 0);
    if (result) {
        logger()->logError("%s PluginInstanceAUv2::configure failed to reset instance!?: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    sampleRate((double) configuration.sampleRate);
    host_transport_info.sampleRate = static_cast<double>(configuration.sampleRate);

    audio_content_type = configuration.dataType;

    auto ret = audio_buses->configure(configuration);
    if (ret != StatusCode::OK)
        return ret;

    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &configuration.offlineMode, sizeof(bool));
    if (result != noErr) {
        logger()->logWarning("%s: configure() on PluginInstanceAU failed to set offlineMode. Status: %d", name.c_str(), result);
    }

    UInt32 frameSize = (UInt32) configuration.bufferSizeInSamples;
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frameSize, sizeof (frameSize));
    if (result) {
        logger()->logError("%s: PluginInstanceAUv2::configure failed to set kAudioUnitProperty_MaximumFramesPerSlice: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    // audio input retriever
    if (!audio_buses->audioInputBuses().empty()) {
        audio_render_callback.inputProc = audioInputRenderCallback;
        audio_render_callback.inputProcRefCon = this;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &audio_render_callback, sizeof(audio_render_callback));
        if (result) {
            logger()->logError("%s: PluginInstanceAUv2::configure failed to set kAudioUnitProperty_SetRenderCallback: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    // MIDI callback
    if (audio_buses->hasEventOutputs()) {
        AUMIDIOutputCallbackStruct callback;
        callback.midiOutputCallback = midiOutputCallback;
        callback.userData = this;
        result = AudioUnitSetProperty (instance, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, &callback, sizeof (callback));
        if (result) {
            logger()->logError("%s: PluginInstanceAUv2::configure failed to set kAudioUnitProperty_MIDIOutputCallback: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    AudioUnitGetProperty(instance, kAudioUnitProperty_InPlaceProcessing, kAudioUnitScope_Global, 0, &process_replacing, &size);

    for (int32_t i = 0, n = audio_buses->audioInputBuses().size(); i < n; i++) {
        // FIXME: get precise channel count
        int numChannels = 2;
        auto b = (AudioBufferList*) calloc(1, sizeof(AudioBufferList) + sizeof(::AudioBuffer) * (numChannels - 1));
        b->mNumberBuffers = numChannels;
        auDataIns.emplace_back(b);
    }
    for (int32_t i = 0, n = audio_buses->audioOutputBuses().size(); i < n; i++) {
        // FIXME: get precise channel count
        int numChannels = 2;
        auto b = (AudioBufferList*) calloc(1, sizeof(AudioBufferList) + sizeof(::AudioBuffer) * (numChannels - 1));
        b->mNumberBuffers = numChannels;
        auDataOuts.emplace_back(b);
    }
    process_timestamp.mSampleTime = 0;

    // Once everything is set, initialize the instance here.
    result = AudioUnitInitialize(instance);
    if (result) {
        logger()->logError("%s: PluginInstanceAUv2::configure failed to initialize AudioUnit: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    return StatusCode::OK;
}


remidy::StatusCode remidy::PluginInstanceAUv2::startProcessing() {
    process_timestamp.mSampleTime = 0;
    host_transport_info.currentSample = 0.0;
    host_transport_info.currentBeat = 0.0;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv2::stopProcessing() {
    host_transport_info.isPlaying = false;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv2::process(AudioProcessContext &process) {

    // It seems the AudioUnit framework resets this information every time...

    bool useDouble = audio_content_type == AudioContentType::Float64;
    UInt32 sampleSize = useDouble ? sizeof(double) : sizeof(float);
    uint32_t channelBufSize = process.frameCount() * sampleSize;
    // FIXME: we still don't initialize non-main buses, so only deal with the main bus so far.
    //for (size_t bus = 0, n = auDataIns.size(); bus < n; bus++) {
    if (!auDataIns.empty()) {
        size_t bus = 0;
        auto auDataIn = auDataIns[bus];
        auto numChannels = process.inputChannelCount(bus);
        auDataIn->mNumberBuffers = numChannels;
        for (int32_t ch = 0; ch < numChannels; ch++) {
            auDataIn->mBuffers[ch].mNumberChannels = 1;
            auDataIn->mBuffers[ch].mData = useDouble ? (void*) process.getDoubleInBuffer(bus, ch) :
                                           process.getFloatInBuffer(bus, ch);
            auDataIn->mBuffers[ch].mDataByteSize = channelBufSize;
        }
    }

    // Update transport info from MasterContext
    auto* trackContext = process.trackContext();
    auto& masterContext = trackContext->masterContext();

    // process_timestamp.mSampleTime is maintained locally and advances continuously
    // This is critical: AU plugins need to see time advancing to process MIDI events,
    // even when the DAW transport is stopped
    process_timestamp.mHostTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    process_timestamp.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;

    // Report transport state - use actual playback position for transport callbacks
    host_transport_info.isPlaying = masterContext.isPlaying();
    host_transport_info.transportStateChanged = false;
    host_transport_info.currentSample = static_cast<Float64>(masterContext.playbackPositionSamples());
    host_transport_info.sampleRate = masterContext.sampleRate();

    // tempo in masterContext is in microseconds per quarter note, convert to BPM
    double tempoBPM = 60000000.0 / masterContext.tempo();
    host_transport_info.currentTempo = tempoBPM;

    if (host_transport_info.currentTempo > 0.0 && host_transport_info.sampleRate > 0.0) {
        double seconds = static_cast<double>(masterContext.playbackPositionSamples()) / host_transport_info.sampleRate;
        host_transport_info.currentBeat = seconds * (host_transport_info.currentTempo / 60.0);
    } else {
        host_transport_info.currentBeat = 0.0;
    }

    // FIXME: we still don't initialize non-main buses, so only deal with the main bus so far.
    //for (size_t bus = 0, n = auDataOuts.size(); bus < n; bus++, bus++) {
    if (!auDataOuts.empty()) {
        size_t bus = 0;
        auto auDataOut = auDataOuts[bus];
        auto numChannels = process.outputChannelCount(bus);
        auDataOut->mNumberBuffers = numChannels;
        for (int32_t ch = 0; ch < numChannels; ch++) {
            auDataOut->mBuffers[ch].mNumberChannels = 1;
            auDataOut->mBuffers[ch].mData = useDouble ?
                                            (void*) process.getDoubleOutBuffer(bus, ch) :
                                            process.getFloatOutBuffer(bus, ch);
            auDataOut->mBuffers[ch].mDataByteSize = channelBufSize;
        }

        if (audio_buses->hasEventInputs())
            // FIXME: pass correct timestamp
            ump_input_dispatcher.process(0, process);

        // Clear output buffer before processing
        midi_output_count = 0;

        AudioUnitRenderActionFlags flags = 0;
        auto status = AudioUnitRender(instance, &flags, &process_timestamp, 0, process.frameCount(), auDataOut);
        if (status != noErr) {
            logger()->logError("%s: failed to process audio PluginInstanceAUv2::process(). Status: %d", name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }

        // Advance timestamp for next process() call - audio is flowing even when transport is stopped
        process_timestamp.mSampleTime += process.frameCount();

        // Copy MIDI output events from temporary buffer to process context
        if (midi_output_count > 0) {
            auto& eventOut = process.eventOut();
            auto* umpBuffer = static_cast<uint32_t*>(eventOut.getMessages());
            size_t umpPosition = eventOut.position() / sizeof(uint32_t);
            size_t umpCapacity = eventOut.maxMessagesInBytes() / sizeof(uint32_t);

            size_t copyCount = std::min(midi_output_count, umpCapacity - umpPosition);
            if (copyCount > 0) {
                std::memcpy(&umpBuffer[umpPosition], midi_output_buffer.data(), copyCount * sizeof(uint32_t));
                eventOut.position((umpPosition + copyCount) * sizeof(uint32_t));
            }

            // Clear temporary buffer
            midi_output_count = 0;
        }
    }

    return StatusCode::OK;
}

// AudioPluginInstanceAUv2

remidy::StatusCode remidy::PluginInstanceAUv2::sampleRate(double sampleRate) {
    UInt32* data;
    UInt32 size;

    if (audioUnitHasIO(instance, kAudioUnitScope_Input)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, sizeof(double));
        if (result != noErr) {
            logger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set input sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    if (audioUnitHasIO(instance, kAudioUnitScope_Output)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate, sizeof(double));
        if (result != noErr) {
            logger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set output sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    return StatusCode::OK;
}

// AudioPluginInstanceAUv3

#if REMIDY_LEGACY_AUV3
remidy::StatusCode remidy::PluginInstanceAUv3::sampleRate(double sampleRate) {
    // FIXME: implement
    logger()->logWarning("AudioPluginInstanceAUv3::sampleRate() not implemented");
    return StatusCode::OK;
}
#endif

OSStatus remidy::PluginInstanceAUv2::hostCallbackGetBeatAndTempo(void* inHostUserData, Float64* outCurrentBeat, Float64* outCurrentTempo) {
    auto* instance = static_cast<PluginInstanceAUv2*>(inHostUserData);
    if (!instance)
        return kAudio_ParamError;

    if (outCurrentBeat)
        *outCurrentBeat = instance->host_transport_info.currentBeat;
    if (outCurrentTempo)
        *outCurrentTempo = instance->host_transport_info.currentTempo;

    return noErr;
}

OSStatus remidy::PluginInstanceAUv2::hostCallbackGetMusicalTimeLocation(
    void* inHostUserData,
    UInt32* outDeltaSampleOffsetToNextBeat,
    Float32* outTimeSigNumerator,
    UInt32* outTimeSigDenominator,
    Float64* outCurrentMeasureDownBeat) {

    auto* instance = static_cast<PluginInstanceAUv2*>(inHostUserData);
    if (!instance)
        return kAudio_ParamError;

    auto& info = instance->host_transport_info;

    if (outDeltaSampleOffsetToNextBeat) {
        if (info.currentTempo > 0.0 && info.sampleRate > 0.0) {
            double samplesPerBeat = info.sampleRate * 60.0 / info.currentTempo;
            double beatFraction = std::fmod(info.currentBeat, 1.0);
            double remainingBeats = (beatFraction > 0.0) ? (1.0 - beatFraction) : 0.0;
            *outDeltaSampleOffsetToNextBeat = static_cast<UInt32>(remainingBeats * samplesPerBeat);
        } else {
            *outDeltaSampleOffsetToNextBeat = 0;
        }
    }
    if (outTimeSigNumerator)
        *outTimeSigNumerator = static_cast<Float32>(info.timeSigNumerator);
    if (outTimeSigDenominator)
        *outTimeSigDenominator = info.timeSigDenominator;
    if (outCurrentMeasureDownBeat) {
        if (info.timeSigNumerator > 0) {
            double measureIndex = std::floor(info.currentBeat / static_cast<double>(info.timeSigNumerator));
            *outCurrentMeasureDownBeat = measureIndex * static_cast<double>(info.timeSigNumerator);
        } else {
            *outCurrentMeasureDownBeat = 0.0;
        }
    }

    return noErr;
}

OSStatus remidy::PluginInstanceAUv2::hostCallbackGetTransportState(
    void* inHostUserData,
    Boolean* outIsPlaying,
    Boolean* outTransportStateChanged,
    Float64* outCurrentSampleInTimeline,
    Boolean* outIsCycling,
    Float64* outCycleStartBeat,
    Float64* outCycleEndBeat) {

    auto* instance = static_cast<PluginInstanceAUv2*>(inHostUserData);
    if (!instance)
        return kAudio_ParamError;

    auto& info = instance->host_transport_info;

    if (outIsPlaying)
        *outIsPlaying = info.isPlaying ? 1 : 0;
    if (outTransportStateChanged)
        *outTransportStateChanged = info.transportStateChanged ? 1 : 0;
    if (outCurrentSampleInTimeline)
        *outCurrentSampleInTimeline = info.currentSample;
    if (outIsCycling)
        *outIsCycling = 0;
    if (outCycleStartBeat)
        *outCycleStartBeat = info.cycleStart;
    if (outCycleEndBeat)
        *outCycleEndBeat = info.cycleEnd;

    return noErr;
}

OSStatus remidy::PluginInstanceAUv2::hostCallbackGetTransportState2(
    void* inHostUserData,
    Boolean* outIsPlaying,
    Boolean* outIsRecording,
    Boolean* outTransportStateChanged,
    Float64* outCurrentSampleInTimeline,
    Boolean* outIsCycling,
    Float64* outCycleStartBeat,
    Float64* outCycleEndBeat) {

    auto* instance = static_cast<PluginInstanceAUv2*>(inHostUserData);
    if (!instance)
        return kAudio_ParamError;

    auto& info = instance->host_transport_info;

    if (outIsPlaying)
        *outIsPlaying = info.isPlaying ? 1 : 0;
    if (outIsRecording)
        *outIsRecording = info.isRecording ? 1 : 0;
    if (outTransportStateChanged)
        *outTransportStateChanged = info.transportStateChanged ? 1 : 0;
    if (outCurrentSampleInTimeline)
        *outCurrentSampleInTimeline = info.currentSample;
    if (outIsCycling)
        *outIsCycling = 0;
    if (outCycleStartBeat)
        *outCycleStartBeat = info.cycleStart;
    if (outCycleEndBeat)
        *outCycleEndBeat = info.cycleEnd;

    return noErr;
}

#endif
