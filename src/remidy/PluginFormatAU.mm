#if __APPLE__

#include "PluginFormatAU.hpp"
#include <cmidi2.h>

remidy::PluginFormatAU::PluginFormatAU() {
    impl = new Impl(this, Logger::global());
}

remidy::PluginFormatAU::~PluginFormatAU() {
    delete impl;
}

remidy::Logger* remidy::PluginFormatAU::getLogger() {
    return impl->getLogger();
}

remidy::PluginExtensibility<remidy::PluginFormat> * remidy::PluginFormatAU::getExtensibility() {
    return impl->getExtensibility();
}

remidy::PluginScanner* remidy::PluginFormatAU::scanner() {
    return impl->scanner();
}

struct AUPluginEntry {
    AudioComponent component;
    UInt32 flags;
    std::string id;
    std::string name;
    std::string vendor;
};

std::string cfStringToString1024(CFStringRef s) {
    char buf[1024];
    CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
    std::string ret{buf};
    return ret;
}

std::string retrieveCFStringRelease(const std::function<void(CFStringRef&)>&& retriever) {
    CFStringRef cf;
    retriever(cf);
    auto ret = cfStringToString1024(cf);
    CFRelease(cf);
    return ret;
}

std::vector<AUPluginEntry> scanAllAvailableAUPlugins() {
    std::vector<AUPluginEntry> ret{};
    AudioComponent component{nullptr};

    while(true) {
        AudioComponentDescription desc{};
        component = AudioComponentFindNext(component, &desc);
        if (!component)
            return ret;
        AudioComponentGetDescription(component, &desc);

        switch (desc.componentType) {
            case kAudioUnitType_MusicDevice:
            case kAudioUnitType_Effect:
            case kAudioUnitType_Generator:
            case kAudioUnitType_MusicEffect:
            case kAudioUnitType_MIDIProcessor:
                break;
            default:
                continue;
        }

        // LAMESPEC: it is quite hacky and lame expectation that the AudioComponent `name` always has `: ` ...
        auto name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
        auto firstColon = name.find_first_of(':');
        auto vendor = name.substr(0, firstColon);
        auto pluginName = name.substr(firstColon + 2); // remaining after ": "
        std::ostringstream id{};
        id << std::hex << desc.componentManufacturer << " " << desc.componentType << " " << desc.componentSubType;

        ret.emplace_back(AUPluginEntry{component, desc.componentFlags, id.str(),
            pluginName,
            vendor
        });
    }
    return ret;
}

std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> remidy::PluginScannerAU::scanAllAvailablePlugins() {
    std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};

    for (auto& plugin : scanAllAvailableAUPlugins()) {
        auto entry = std::make_unique<PluginCatalogEntry>();
        static std::string format{"AU"};
        entry->format(format);
        entry->pluginId(plugin.id);
        entry->displayName(plugin.name);
        entry->vendorName(plugin.vendor);
        // no product URL in the queryable metadata.

        ret.emplace_back(std::move(entry));
    }
    return ret;
}

void remidy::PluginFormatAU::createInstance(PluginCatalogEntry* info, std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback) {
    AudioComponentDescription desc{};
    std::istringstream id{info->pluginId()};
    id >> std::hex >> std::setw(2) >> desc.componentManufacturer >> desc.componentType >> desc.componentSubType;

    auto component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        callback(nullptr, "The specified AudioUnit component was not found");
    }

    AudioComponentGetDescription(component, &desc);
    bool v3 = (desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) > 0;
    AudioComponentInstantiationOptions options = 0;

    __block auto cb = std::move(callback);
    AudioComponentInstantiate(component, options, ^(AudioComponentInstance instance, OSStatus status) {
        if (status == noErr) {
            if (v3) {
                auto au = std::make_unique<AudioPluginInstanceAUv3>(this, info, component, instance);
                cb(std::move(au), "");
            } else {
                auto au = std::make_unique<AudioPluginInstanceAUv2>(this, info, component, instance);
                cb(std::move(au), "");
            }
        }
        else
          callback(nullptr, std::format("Failed to instantiate AudioUnit {}", info->displayName()));
    });
}

remidy::PluginFormatAU::Extensibility::Extensibility(PluginFormat &format) : PluginExtensibility(format) {
}


// AudioPluginInstanceAU

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(PluginFormatAU *format, PluginCatalogEntry* info, AudioComponent component, AudioComponentInstance instance) :
    PluginInstance(info), format(format), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
    setCurrentThreadNameIfPossible("remidy.AU.instance." + name);
    inspectBuses();
}

remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
    if (_parameters)
        delete _parameters;
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
        format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to reset instance!?: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    sampleRate((double) configuration.sampleRate);

    // FIXME: there seems some misunderstandings on either how we represent channel or
    // how we should copy audio buffer.
    audio_content_type = configuration.dataType;
    UInt32 sampleSize = configuration.dataType == AudioContentType::Float64 ? sizeof(double) : sizeof(float);
    AudioStreamBasicDescription stream{};
    for (auto i = 0; i < buses.numAudioIn; i++) {
        result = AudioUnitGetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i,
                                      &stream, &size);
        if (result == noErr) { // some plugins do not seem to support this property...
            stream.mSampleRate = configuration.sampleRate;
            stream.mFormatID = kAudioFormatLinearPCM;
            stream.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
            stream.mBitsPerChannel = 8 * sampleSize;
            stream.mFramesPerPacket = 1;
            stream.mBytesPerFrame = sampleSize;
            stream.mBytesPerPacket = sampleSize;
            // FIXME: retrieve from bus
            stream.mChannelsPerFrame = 2;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set input kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                return StatusCode::FAILED_TO_CONFIGURE;
            }
        }

        /*
        ::AudioChannelLayout auLayout{};
        // FIXME: retrieve from bus
        auLayout.mChannelBitmap = kAudioChannelBit_Left | kAudioChannelBit_Right;
        auLayout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        UInt32 size;
        result = AudioUnitGetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, i, &auLayout, &size);
        //result = AudioUnitSetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, i, &auLayout, sizeof(::AudioChannelLayout));
        if (result) {
            format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set input kAudioUnitProperty_AudioChannelLayout: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }*/
    }
    for (auto i = 0; i < buses.numAudioOut; i++) {
        result = AudioUnitGetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i,
                                      &stream, &size);
        if (result == noErr) { // some plugins do not seem to support this property...
            stream.mSampleRate = configuration.sampleRate;
            stream.mFormatID = kAudioFormatLinearPCM;
            stream.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
            stream.mBitsPerChannel = 8 * sampleSize;
            stream.mFramesPerPacket = 1;
            stream.mBytesPerFrame = sampleSize;
            stream.mBytesPerPacket = sampleSize;
            // FIXME: retrieve from bus
            stream.mChannelsPerFrame = 2;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                return StatusCode::FAILED_TO_CONFIGURE;
            }
        }

        /*
        ::AudioChannelLayout auLayout{};
        // FIXME: retrieve from bus
        auLayout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, i, &auLayout, sizeof(::AudioChannelLayout));
        if (result) {
            format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_AudioChannelLayout: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }*/
    }

    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &configuration.offlineMode, sizeof(bool));
    if (result != noErr) {
        this->format->getLogger()->logWarning("%s: configure() on AudioPluginInstanceAU failed to set offlineMode. Status: %d", name.c_str(), result);
    }

    UInt32 frameSize = (UInt32) configuration.bufferSizeInSamples;
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frameSize, sizeof (frameSize));
    if (result) {
        format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_MaximumFramesPerSlice: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    // audio input retriever
    if (!input_buses.empty()) {
        AURenderCallbackStruct callback;
        callback.inputProc = audioInputRenderCallback;
        callback.inputProcRefCon = this;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        if (result) {
            format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_SetRenderCallback: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    // MIDI callback
    if (buses.hasMidiIn) {
        AUMIDIOutputCallbackStruct callback;
        callback.midiOutputCallback = midiOutputCallback;
        callback.userData = this;
        result = AudioUnitSetProperty (instance, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0, &callback, sizeof (callback));
        if (result) {
            format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_MIDIOutputCallback: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    AudioUnitGetProperty(instance, kAudioUnitProperty_InPlaceProcessing, kAudioUnitScope_Global, 0, &process_replacing, &size);

    for (int32_t i = 0; i < buses.numAudioIn; i++) {
        // FIXME: get precise channel count
        int numChannels = 2;
        auto b = (AudioBufferList*) calloc(1, sizeof(AudioBufferList) + sizeof(::AudioBuffer) * (numChannels - 1));
        b->mNumberBuffers = numChannels;
        auDataIns.emplace_back(b);
    }
    for (int32_t i = 0; i < buses.numAudioOut; i++) {
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
        format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to initialize AudioUnit: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::startProcessing() {
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
    for (int32_t bus = 0, n = process.audioInBusCount(); bus < n; bus++) {
        auto auDataIn = auDataIns[bus];
        auDataIn->mNumberBuffers = 0;
        auto busBuf =process.audioIn(0);
        auto numChannels = busBuf->channelCount();
        auDataIn->mBuffers[bus].mNumberChannels = numChannels;
        for (int32_t ch = 0; ch < busBuf->channelCount(); ch++) {
            auDataIn->mBuffers[ch].mData = useDouble ?
                (void*) busBuf->getDoubleBufferForChannel(ch) :
                busBuf->getFloatBufferForChannel(ch);
            auDataIn->mBuffers[ch].mDataByteSize = channelBufSize;
            auDataIn->mNumberBuffers++;
        }
    }

    process_timestamp.mHostTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    process_timestamp.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;

    for (int32_t bus = 0, n = process.audioOutBusCount(); bus < n; bus++, bus++) {
        auto auDataOut = auDataOuts[bus];
        auDataOut->mNumberBuffers = 0;
        auto busBuf =process.audioOut(0);
        auto numChannels = busBuf->channelCount();
        auDataOut->mBuffers[bus].mNumberChannels = numChannels;
        for (int32_t ch = 0; ch < busBuf->channelCount(); ch++) {
            auDataOut->mBuffers[ch].mData = useDouble ?
                (void*) busBuf->getDoubleBufferForChannel(ch) :
                busBuf->getFloatBufferForChannel(ch);
            auDataOut->mBuffers[ch].mDataByteSize = channelBufSize;
            auDataOut->mNumberBuffers++;
        }

        if (buses.hasMidiIn)
            ump_input_dispatcher.process(0, process);

        auto status = AudioUnitRender(instance, nullptr, &process_timestamp, 0, process.frameCount(), auDataOut);
        if (status != noErr) {
            format->getLogger()->logError("%s: failed to process audio AudioPluginInstanceAU::process(). Status: %d", name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }
    }
    process_timestamp.mSampleTime += process.frameCount();

    return StatusCode::OK;
}

void remidy::AudioPluginInstanceAU::inspectBuses() {
    OSStatus result;
    BusSearchResult ret{};
    ::AudioChannelLayout layout{};
    UInt32 count{};
    UInt32 size = sizeof(UInt32);
    result = AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &count, &size);
    if (result)
        format->getLogger()->logWarning("%s: failed to retrieve input kAudioUnitProperty_ElementCount. Status: %d", name.c_str(), result);
    else
        ret.numAudioIn = count;
    result = AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &count, &size);
    if (result)
        format->getLogger()->logWarning("%s: failed to retrieve output kAudioUnitProperty_ElementCount. Status: %d", name.c_str(), result);
    else
        ret.numAudioOut = count;

    // FIXME: we need to fill input_buses and output_buses here.

    for (auto bus : input_buses)
      delete bus;
    for (auto bus : output_buses)
      delete bus;
    input_buses.clear();
    output_buses.clear();
    input_bus_defs.clear();
    output_bus_defs.clear();

    ::AudioChannelLayout auLayout;
    for (auto i = 0; i < ret.numAudioIn; i++) {
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, i, &auLayout, &size) == noErr) {
            CFStringRef cfName{nullptr};
            if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementName, kAudioUnitScope_Input, i, &cfName, &size) == noErr && cfName != nullptr) {
                // FIXME: get bus name
                auto busName = std::string{""};//cfStringToString1024(cfName);
                AudioBusDefinition def{busName, AudioBusRole::Main}; // FIXME: correct bus type
                // FIXME: fill channel layouts
                // also use AudioChannelLayoutTag_GetNumberOfChannels(auLayout)
                input_bus_defs.emplace_back(def);
                input_buses.emplace_back(new AudioBusConfiguration(def));
            }
        }
    }

    AudioComponentDescription desc;
    AudioComponentGetDescription(component, &desc);
    switch (desc.componentType) {
        case kAudioUnitType_MusicDevice:
        case kAudioUnitType_MusicEffect:
        case kAudioUnitType_MIDIProcessor:
          ret.hasMidiIn = true;
    }
    Boolean writable;
    auto status = AudioUnitGetPropertyInfo(instance, kAudioUnitProperty_MIDIOutputCallbackInfo, kAudioUnitScope_Global, 0, nullptr, &writable);
    if (status == noErr && writable)
        ret.hasMidiOut = true;

    buses = ret;
}

const std::vector<remidy::AudioBusConfiguration*> remidy::AudioPluginInstanceAU::audioInputBuses() const { return input_buses; }
const std::vector<remidy::AudioBusConfiguration*> remidy::AudioPluginInstanceAU::audioOutputBuses() const { return output_buses; }

// AudioPluginInstanceAUv2

remidy::StatusCode remidy::AudioPluginInstanceAUv2::sampleRate(double sampleRate) {
    UInt32* data;
    UInt32 size;

    if (audioUnitHasIO(instance, kAudioUnitScope_Input)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, sizeof(double));
        if (result != noErr) {
            this->format->getLogger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set input sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    if (audioUnitHasIO(instance, kAudioUnitScope_Output)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate, sizeof(double));
        if (result != noErr) {
            this->format->getLogger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set output sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    return StatusCode::OK;
}

// AudioPluginInstanceAUv3

remidy::StatusCode remidy::AudioPluginInstanceAUv3::sampleRate(double sampleRate) {
    // FIXME: implement
    format->getLogger()->logWarning("AudioPluginInstanceAUv3::sampleRate() not implemented");
    return StatusCode::OK;
}

// AudioPluginInstanceAU::ParameterSupport

remidy::AudioPluginInstanceAU::ParameterSupport::ParameterSupport(remidy::AudioPluginInstanceAU *owner)
    : owner(owner) {
    AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &parameter_id_list, &parameter_list_size);
    for (size_t i = 0, n = parameter_list_size / sizeof(AudioUnitParameterID); i < n; i++) {
        auto id = parameter_id_list[i];
        AudioUnitParameterInfo info;
        UInt32 size;
        AudioUnitGetProperty(owner->instance, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, i, &info, &size);
        std::string idString = std::format("{}", id);
        std::string name{info.name};
        std::string path{};
        auto p = new PluginParameter(idString, name, path, info.defaultValue, info.minValue, info.maxValue, true);
        parameter_list.emplace_back(p);
    }
}

std::vector<remidy::PluginParameter*> remidy::AudioPluginInstanceAU::ParameterSupport::parameters() {
    return parameter_list;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::setParameter(uint32_t index, double value, uint64_t timestamp) {
    // FIXME: calculate inBufferOffsetInFrames from timestamp.
    auto inBufferOffsetInFrames = 0;
    AudioUnitSetParameter(owner->instance, parameter_id_list[index], kAudioUnitScope_Global, 0, value, inBufferOffsetInFrames);
    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::ParameterSupport::getParameter(uint32_t index, double* value) {
    AudioUnitParameterValue av;
    AudioUnitGetParameter(owner->instance, parameter_id_list[index], kAudioUnitScope_Global, 0, &av);
    *value = av;
    return StatusCode::OK;
}

#endif

remidy::AudioPluginInstanceAU::AUUmpInputDispatcher::AUUmpInputDispatcher(remidy::AudioPluginInstanceAU *owner) :
    owner(owner)
{
    #define AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE 65536
    // FIXME: assign MIDI buffer size somewhere
    ump_event_list = (MIDIEventList*) calloc(AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE, 1);
}
remidy::AudioPluginInstanceAU::AUUmpInputDispatcher::~AUUmpInputDispatcher() {
    if (ump_event_list)
        free(ump_event_list);
}

void
remidy::AudioPluginInstanceAU::AUUmpInputDispatcher::process(uint64_t timestamp, remidy::AudioProcessContext &src) {
    auto midiIn = src.midiIn();
    auto cur = MIDIEventListInit(ump_event_list, kMIDIProtocol_2_0);
    MIDIEventListAdd(ump_event_list, AU_UMP_INPUT_DISPATCHER_UMP_EVENT_LIST_SIZE,
                     cur, timestamp, midiIn.sizeInInts(), midiIn.getMessages());
    auto result = MusicDeviceMIDIEventList((MusicDeviceComponent) owner->instance, timestamp, ump_event_list);
    if (result != noErr) {
        owner->logger()->logError("Failed to add UMP events to AudioUnit: %d", result);
    }
}
