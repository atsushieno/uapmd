#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include "auv2/AUv2Helper.hpp"
#include <AVFoundation/AVFoundation.h>
#include <CoreFoundation/CoreFoundation.h>



namespace remidy {
    class AudioPluginFormatAU::Impl {
        AudioPluginFormat* format;
        Logger* logger;
        Extensibility extensibility;
    public:
        Impl(AudioPluginFormat* format, Logger* logger) : format(format), logger(logger), extensibility(*format) {}

        Extensibility* getExtensibility() { return &extensibility; }
        Logger* getLogger() { return logger; }
    };

    class AudioPluginInstanceAU : public AudioPluginInstance {
        struct BusSearchResult {
            uint32_t numAudioIn{0};
            uint32_t numAudioOut{0};
            bool hasMidiIn{false};
            bool hasMidiOut{false};
        };
        BusSearchResult buses{};
        BusSearchResult inspectBuses();
        std::vector<AudioBusConfiguration*> input_buses;
        std::vector<AudioBusConfiguration*> output_buses;

        OSStatus audioInputRenderCallback(AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData);
        static OSStatus audioInputRenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData) {
            return ((AudioPluginInstanceAU *)inRefCon)->audioInputRenderCallback(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
        }
        OSStatus midiOutputCallback(const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist);
        static OSStatus midiOutputCallback(void *userData, const AudioTimeStamp *timeStamp, UInt32 midiOutNum, const struct MIDIPacketList *pktlist) {
            return ((remidy::AudioPluginInstanceAU*) userData)->midiOutputCallback(timeStamp, midiOutNum, pktlist);
        }

        std::vector<::AudioBufferList*> auDataIns{};
        std::vector<::AudioBufferList*> auDataOuts{};
        AudioTimeStamp process_timestamp{};
        bool process_replacing{false};
        AudioContentType audio_content_type{AudioContentType::Float32};

    protected:
        AudioPluginFormatAU *format;
        PluginCatalogEntry info;
        AudioComponent component;
        AudioUnit instance;
        std::string name{};

        AudioPluginInstanceAU(AudioPluginFormatAU* format, PluginCatalogEntry& info, AudioComponent component, AudioUnit instance);
        ~AudioPluginInstanceAU() override;

    public:
        enum AUVersion {
            AUV2 = 2,
            AUV3 = 3
        };

        AudioPluginUIThreadRequirement requiresUIThreadOn() override {
            // maybe we add some entries for known issues
            return format->requiresUIThreadOn(&info);
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode process(AudioProcessContext &process) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

        // port helpers
        bool hasAudioInputs() override { return buses.numAudioIn > 0; }
        bool hasAudioOutputs() override { return buses.numAudioOut > 0; }
        bool hasEventInputs() override { return buses.hasMidiIn; }
        bool hasEventOutputs() override { return buses.hasMidiOut; }

        const std::vector<AudioBusConfiguration*> audioInputBuses() const override;
        const std::vector<AudioBusConfiguration*> audioOutputBuses() const override;

        virtual AUVersion auVersion() = 0;
        virtual StatusCode sampleRate(double sampleRate) = 0;
    };

    class AudioPluginInstanceAUv2 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv2(AudioPluginFormatAU* format, PluginCatalogEntry& info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, info, component, instance) {
        }

        ~AudioPluginInstanceAUv2() override = default;

        AUVersion auVersion() override { return AUV2; }

        StatusCode sampleRate(double sampleRate) override;
    };

    class AudioPluginInstanceAUv3 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv3(AudioPluginFormatAU* format, PluginCatalogEntry& info, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, info, component, instance) {
        }

        ~AudioPluginInstanceAUv3() override = default;

        AUVersion auVersion() override { return AUV3; }

        StatusCode sampleRate(double sampleRate) override;
    };
}

remidy::AudioPluginFormatAU::AudioPluginFormatAU() {
    impl = new Impl(this, Logger::global());
}

remidy::AudioPluginFormatAU::~AudioPluginFormatAU() {
    delete impl;
}

remidy::Logger* remidy::AudioPluginFormatAU::getLogger() {
    return impl->getLogger();
}

remidy::AudioPluginExtensibility<remidy::AudioPluginFormat> * remidy::AudioPluginFormatAU::getExtensibility() {
    return impl->getExtensibility();
}

std::vector<std::filesystem::path> ret{};
std::vector<std::filesystem::path>& remidy::AudioPluginFormatAU::getDefaultSearchPaths() {
    ret.clear();
    return ret;
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
        AudioComponentGetDescription(component, &desc);
        if (!component)
            return ret;

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

std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> remidy::AudioPluginFormatAU::scanAllAvailablePlugins() {
    std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};

    for (auto& plugin : scanAllAvailableAUPlugins()) {
        auto entry = std::make_unique<PluginCatalogEntry>();
        static std::string format{"AU"};
        entry->format(format);
        entry->pluginId(plugin.id);
        entry->setMetadataProperty(PluginCatalogEntry::DisplayName, plugin.name);
        entry->setMetadataProperty(PluginCatalogEntry::VendorName, plugin.vendor);
        ret.emplace_back(std::move(entry));
    }
    return ret;
}

void remidy::AudioPluginFormatAU::createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) {
    AudioComponentDescription desc{};
    std::istringstream id{info->pluginId()};
    id >> std::hex >> std::setw(2) >> desc.componentManufacturer >> desc.componentType >> desc.componentSubType;

    auto component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        callback(InvokeResult{nullptr, std::string{"The specified AudioUnit component was not found"}});
    }

    AudioComponentGetDescription(component, &desc);
    bool v3 = (desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) > 0;
    AudioComponentInstantiationOptions options = 0;

    AudioComponentInstantiate(component, options, ^(AudioComponentInstance instance, OSStatus status) {
        if (status == noErr) {
            if (v3)
                callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv3>(this, *info, component, instance), std::string{}});
            else
                callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv2>(this, *info, component, instance), std::string{}});
        }
        else
          callback(InvokeResult{nullptr, std::string("Failed to instantiate AudioUnit.")});
    });
}

remidy::AudioPluginFormatAU::Extensibility::Extensibility(AudioPluginFormat &format) : AudioPluginExtensibility(format) {
}


// AudioPluginInstanceAU

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(AudioPluginFormatAU *format, PluginCatalogEntry& info, AudioComponent component, AudioComponentInstance instance) :
    format(format), info(info), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
    buses = inspectBuses();
}

remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
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
    stream.mSampleRate = configuration.sampleRate;
    stream.mFormatID = kAudioFormatLinearPCM;
    stream.mFormatFlags = kAudioFormatFlagsAudioUnitCanonical;
    stream.mBitsPerChannel = 8 * sampleSize;
    stream.mFramesPerPacket = 1;
    stream.mBytesPerFrame = sampleSize;
    stream.mBytesPerPacket = sampleSize;
    for (auto i = 0; i < buses.numAudioIn; i++) {
        // FIXME: retrieve from bus
        stream.mChannelsPerFrame = 2;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i,
                                      &stream, sizeof(AudioStreamBasicDescription));
        if (result) {
            format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set input kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
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
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i,
                                      &stream, sizeof(AudioStreamBasicDescription));
        if (result) {
            format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
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
    if (hasAudioInputs()) {
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

    UInt32 size;
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

        try {
        auto status = AudioUnitRender(instance, nullptr, &process_timestamp, 0, process.frameCount(), auDataOut);
        if (status != noErr) {
            format->getLogger()->logError("%s: failed to process audio AudioPluginInstanceAU::process(). Status: %d", name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }
        } catch (...) {
            std::cerr << "RUNTIME CRASH" << std::endl;
            return StatusCode::FAILED_TO_PROCESS;
        }
    }
    process_timestamp.mSampleTime += process.frameCount();

    return StatusCode::OK;
}

remidy::AudioPluginInstanceAU::BusSearchResult remidy::AudioPluginInstanceAU::inspectBuses() {
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

    return ret;
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

#endif
