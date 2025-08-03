#if __APPLE__

#include "PluginFormatAU.hpp"

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(PluginFormatAU *format, Logger* logger, PluginCatalogEntry* info, AudioComponent component, AudioComponentInstance instance) :
        PluginInstance(info), format(format), logger_(logger), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
    setCurrentThreadNameIfPossible("remidy.AU.instance." + name);
    audio_buses = new AudioBuses(this);
}

remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
    delete audio_buses;
    delete _parameters;
    delete _states;

    for (auto auDataIn : auDataIns)
        free(auDataIn);
    for (auto auDataOut : auDataOuts)
        free(auDataOut);
    AudioComponentInstanceDispose(instance);
}

OSStatus remidy::AudioPluginInstanceAU::audioInputRenderCallback(
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

OSStatus remidy::AudioPluginInstanceAU::midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
    // FIXME: implement
    std::cerr << "remidy::AudioPluginInstanceAU::midiOutputCallback() not implemented." << std::endl;
    return noErr;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::configure(ConfigurationRequest& configuration) {
    OSStatus result;
    UInt32 size; // unused field for AudioUnitGetProperty

    result = AudioUnitReset(instance, kAudioUnitScope_Global, 0);
    if (result) {
        logger()->logError("%s AudioPluginInstanceAU::configure failed to reset instance!?: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    sampleRate((double) configuration.sampleRate);

    audio_content_type = configuration.dataType;

    auto ret = audio_buses->configure(configuration);
    if (ret != StatusCode::OK)
        return ret;

    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &configuration.offlineMode, sizeof(bool));
    if (result != noErr) {
        logger()->logWarning("%s: configure() on AudioPluginInstanceAU failed to set offlineMode. Status: %d", name.c_str(), result);
    }

    UInt32 frameSize = (UInt32) configuration.bufferSizeInSamples;
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frameSize, sizeof (frameSize));
    if (result) {
        logger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_MaximumFramesPerSlice: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    // audio input retriever
    if (!audio_buses->audioInputBuses().empty()) {
        audio_render_callback.inputProc = audioInputRenderCallback;
        audio_render_callback.inputProcRefCon = this;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &audio_render_callback, sizeof(audio_render_callback));
        if (result) {
            logger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_SetRenderCallback: OSStatus %d", name.c_str(), result);
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
            logger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_MIDIOutputCallback: OSStatus %d", name.c_str(), result);
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
        logger()->logError("%s: AudioPluginInstanceAU::configure failed to initialize AudioUnit: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::startProcessing() {
    process_timestamp.mSampleTime = 0;
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::stopProcessing() {
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::process(AudioProcessContext &process) {

    // It seems the AudioUnit framework resets this information every time...

    bool useDouble = audio_content_type == AudioContentType::Float64;
    UInt32 sampleSize = useDouble ? sizeof(double) : sizeof(float);
    uint32_t channelBufSize = process.frameCount() * sampleSize;
    for (size_t bus = 0, n = auDataIns.size(); bus < n; bus++) {
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

    for (size_t bus = 0, n = auDataOuts.size(); bus < n; bus++, bus++) {
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

        // FIXME: it is likely that audio effects are not working, blocked here.
        auto status = AudioUnitRender(instance, nullptr, &process_timestamp, 0, process.frameCount(), auDataOut);
        if (status != noErr) {
            logger()->logError("%s: failed to process audio AudioPluginInstanceAU::process(). Status: %d", name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }
    }
    process_timestamp.mSampleTime += process.frameCount();

    return StatusCode::OK;
}

// AudioPluginInstanceAUv2

remidy::StatusCode remidy::AudioPluginInstanceAUv2::sampleRate(double sampleRate) {
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

remidy::StatusCode remidy::AudioPluginInstanceAUv3::sampleRate(double sampleRate) {
    // FIXME: implement
    logger()->logWarning("AudioPluginInstanceAUv3::sampleRate() not implemented");
    return StatusCode::OK;
}

#endif
