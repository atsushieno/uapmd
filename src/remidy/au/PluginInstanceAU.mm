#if __APPLE__

#include "PluginFormatAU.hpp"
#include <cmath>

remidy::PluginInstanceAU::PluginInstanceAU(
        PluginFormatAU *format,
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

remidy::PluginInstanceAU::~PluginInstanceAU() {
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

void remidy::PluginInstanceAU::initializeHostCallbacks() {
    host_transport_info = HostTransportInfo{};
    host_callback_info = HostCallbackInfo{};
    host_callback_info.hostUserData = this;
    host_callback_info.beatAndTempoProc = &PluginInstanceAU::hostCallbackGetBeatAndTempo;
    host_callback_info.musicalTimeLocationProc = &PluginInstanceAU::hostCallbackGetMusicalTimeLocation;
    host_callback_info.transportStateProc = &PluginInstanceAU::hostCallbackGetTransportState;
    host_callback_info.transportStateProc2 = &PluginInstanceAU::hostCallbackGetTransportState2;

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

OSStatus remidy::PluginInstanceAU::audioInputRenderCallback(
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

OSStatus remidy::PluginInstanceAU::midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
    // FIXME: implement
    std::cerr << "remidy::PluginInstanceAU::midiOutputCallback() not implemented." << std::endl;
    return noErr;
}

remidy::StatusCode remidy::PluginInstanceAU::configure(ConfigurationRequest& configuration) {
    OSStatus result;
    UInt32 size; // unused field for AudioUnitGetProperty

    result = AudioUnitReset(instance, kAudioUnitScope_Global, 0);
    if (result) {
        logger()->logError("%s PluginInstanceAU::configure failed to reset instance!?: OSStatus %d", name.c_str(), result);
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
        logger()->logError("%s: PluginInstanceAU::configure failed to set kAudioUnitProperty_MaximumFramesPerSlice: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    // audio input retriever
    if (!audio_buses->audioInputBuses().empty()) {
        audio_render_callback.inputProc = audioInputRenderCallback;
        audio_render_callback.inputProcRefCon = this;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &audio_render_callback, sizeof(audio_render_callback));
        if (result) {
            logger()->logError("%s: PluginInstanceAU::configure failed to set kAudioUnitProperty_SetRenderCallback: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    // MIDI callback
    if (audio_buses->hasEventInputs()) {
        AUMIDIOutputCallbackStruct callback;
        callback.midiOutputCallback = midiOutputCallback;
        callback.userData = this;
        result = AudioUnitSetProperty (instance, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, &callback, sizeof (callback));
        if (result) {
            logger()->logError("%s: PluginInstanceAU::configure failed to set kAudioUnitProperty_MIDIOutputCallback: OSStatus %d", name.c_str(), result);
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
        logger()->logError("%s: PluginInstanceAU::configure failed to initialize AudioUnit: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAU::startProcessing() {
    process_timestamp.mSampleTime = 0;
    host_transport_info.currentSample = 0.0;
    host_transport_info.currentBeat = 0.0;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAU::stopProcessing() {
    host_transport_info.isPlaying = false;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAU::process(AudioProcessContext &process) {

    // It seems the AudioUnit framework resets this information every time...

    bool useDouble = audio_content_type == AudioContentType::Float64;
    UInt32 sampleSize = useDouble ? sizeof(double) : sizeof(float);
    uint32_t channelBufSize = process.frameCount() * sampleSize;
    // FIXME: we still don't initialize non-main buses, so only deal with the main bus so far.
    //for (size_t bus = 0, n = auDataIns.size(); bus < n; bus++) {
    if (!auDataIns.empty()) {
        size_t bus = 0;
        auto auDataIn = auDataIns[bus];
        auDataIn->mNumberBuffers = 0;
        auto numChannels = process.inputChannelCount(bus);
        auDataIn->mBuffers[bus].mNumberChannels = numChannels;
        for (int32_t ch = 0; ch < numChannels; ch++) {
            auDataIn->mBuffers[ch].mData = useDouble ? (void*) process.getDoubleInBuffer(bus, ch) :
                                           process.getFloatInBuffer(bus, ch);
            auDataIn->mBuffers[ch].mDataByteSize = channelBufSize;
            auDataIn->mNumberBuffers++;
        }
    }

    process_timestamp.mHostTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    process_timestamp.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;

    host_transport_info.isPlaying = true;
    host_transport_info.transportStateChanged = false;
    host_transport_info.currentSample = process_timestamp.mSampleTime;
    if (host_transport_info.currentTempo > 0.0 && host_transport_info.sampleRate > 0.0) {
        double seconds = process_timestamp.mSampleTime / host_transport_info.sampleRate;
        host_transport_info.currentBeat = seconds * (host_transport_info.currentTempo / 60.0);
    } else {
        host_transport_info.currentBeat = 0.0;
    }

    // FIXME: we still don't initialize non-main buses, so only deal with the main bus so far.
    //for (size_t bus = 0, n = auDataOuts.size(); bus < n; bus++, bus++) {
    if (!auDataOuts.empty()) {
        size_t bus = 0;
        auto auDataOut = auDataOuts[bus];
        auDataOut->mNumberBuffers = 0;
        auto numChannels = process.outputChannelCount(bus);
        auDataOut->mBuffers[bus].mNumberChannels = numChannels;
        for (int32_t ch = 0; ch < numChannels; ch++) {
            auDataOut->mBuffers[ch].mData = useDouble ?
                                            (void*) process.getDoubleOutBuffer(bus, ch) :
                                            process.getFloatOutBuffer(bus, ch);
            auDataOut->mBuffers[ch].mDataByteSize = channelBufSize;
            auDataOut->mNumberBuffers++;
        }

        if (audio_buses->hasEventInputs())
            // FIXME: pass correct timestamp
            ump_input_dispatcher.process(0, process);

        AudioUnitRenderActionFlags flags = 0;
        // FIXME: it is likely that audio effects are not working, blocked here.
        //  JUCE refuses to have different sizes of auDataOut[*].mBuffers[*].mDataByteSize vs. process.frameCount().
        auto status = AudioUnitRender(instance, &flags, &process_timestamp, 0, process.frameCount(), auDataOut);
        if (status != noErr) {
            logger()->logError("%s: failed to process audio PluginInstanceAU::process(). Status: %d", name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }
    }
    process_timestamp.mSampleTime += process.frameCount();
    host_transport_info.currentSample = process_timestamp.mSampleTime;
    if (host_transport_info.currentTempo > 0.0 && host_transport_info.sampleRate > 0.0) {
        double seconds = process_timestamp.mSampleTime / host_transport_info.sampleRate;
        host_transport_info.currentBeat = seconds * (host_transport_info.currentTempo / 60.0);
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

remidy::StatusCode remidy::PluginInstanceAUv3::sampleRate(double sampleRate) {
    // FIXME: implement
    logger()->logWarning("AudioPluginInstanceAUv3::sampleRate() not implemented");
    return StatusCode::OK;
}

OSStatus remidy::PluginInstanceAU::hostCallbackGetBeatAndTempo(void* inHostUserData, Float64* outCurrentBeat, Float64* outCurrentTempo) {
    auto* instance = static_cast<PluginInstanceAU*>(inHostUserData);
    if (!instance)
        return kAudio_ParamError;

    if (outCurrentBeat)
        *outCurrentBeat = instance->host_transport_info.currentBeat;
    if (outCurrentTempo)
        *outCurrentTempo = instance->host_transport_info.currentTempo;

    return noErr;
}

OSStatus remidy::PluginInstanceAU::hostCallbackGetMusicalTimeLocation(
    void* inHostUserData,
    UInt32* outDeltaSampleOffsetToNextBeat,
    Float32* outTimeSigNumerator,
    UInt32* outTimeSigDenominator,
    Float64* outCurrentMeasureDownBeat) {

    auto* instance = static_cast<PluginInstanceAU*>(inHostUserData);
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

OSStatus remidy::PluginInstanceAU::hostCallbackGetTransportState(
    void* inHostUserData,
    Boolean* outIsPlaying,
    Boolean* outTransportStateChanged,
    Float64* outCurrentSampleInTimeline,
    Boolean* outIsCycling,
    Float64* outCycleStartBeat,
    Float64* outCycleEndBeat) {

    auto* instance = static_cast<PluginInstanceAU*>(inHostUserData);
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

OSStatus remidy::PluginInstanceAU::hostCallbackGetTransportState2(
    void* inHostUserData,
    Boolean* outIsPlaying,
    Boolean* outIsRecording,
    Boolean* outTransportStateChanged,
    Float64* outCurrentSampleInTimeline,
    Boolean* outIsCycling,
    Float64* outCycleStartBeat,
    Float64* outCycleEndBeat) {

    auto* instance = static_cast<PluginInstanceAU*>(inHostUserData);
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
