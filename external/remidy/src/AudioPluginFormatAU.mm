#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include "auv2/AUv2Helper.hpp"
#include <AVFoundation/AVFoundation.h>
#include <CoreFoundation/CoreFoundation.h>



namespace remidy {
    class AudioPluginFormatAU::Impl {
        Logger* logger;
    public:
        Impl(Logger* logger) : logger(logger) {}

        Logger* getLogger() { return logger; }
    };

    class AudioPluginInstanceAU : public AudioPluginInstance {
        struct BusSearchResult {
            uint32_t numAudioIn{0};
            uint32_t numAudioOut{0};
            uint32_t numEventIn{0};
            uint32_t numEventOut{0};
        };
        BusSearchResult buses;
        BusSearchResult inspectBuses();
        std::unique_ptr<::AudioBufferList> auData{nullptr};
        AudioTimeStamp process_timestamp{};
        bool process_replacing{false};

    protected:
        AudioPluginFormatAU *format;
        AudioComponent component;
        AudioUnit instance;
        std::string name;

        AudioPluginInstanceAU(AudioPluginFormatAU* format, AudioComponent component, AudioUnit instance);
        ~AudioPluginInstanceAU() override;

    public:
        enum AUVersion {
            AUV2 = 2,
            AUV3 = 3
        };

        AudioPluginUIThreadRequirement requiresUIThreadOn() override {
            return AudioPluginUIThreadRequirement::None;
        }

        // audio processing core functions.
        StatusCode configure(ConfigurationRequest& configuration) override;
        StatusCode process(AudioProcessContext &process) override;
        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

        // port helpers
        bool hasAudioInputs() override { return buses.numAudioIn > 0; }
        bool hasAudioOutputs() override { return buses.numAudioOut > 0; }
        bool hasEventInputs() override { return buses.numEventIn > 0; }
        bool hasEventOutputs() override { return buses.numAudioOut > 0; }

        virtual AUVersion auVersion() = 0;
        virtual StatusCode sampleRate(double sampleRate) = 0;
    };

    class AudioPluginInstanceAUv2 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv2(AudioPluginFormatAU* format, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, component, instance) {
        }

        ~AudioPluginInstanceAUv2() override = default;

        AUVersion auVersion() override { return AUV2; }

        StatusCode sampleRate(double sampleRate) override;
    };

    class AudioPluginInstanceAUv3 final : public AudioPluginInstanceAU {
    public:
        AudioPluginInstanceAUv3(AudioPluginFormatAU* format, AudioComponent component, AudioUnit instance
        ) : AudioPluginInstanceAU(format, component, instance) {
        }

        ~AudioPluginInstanceAUv3() override = default;

        AUVersion auVersion() override { return AUV3; }

        StatusCode sampleRate(double sampleRate) override;
    };
}

remidy::AudioPluginFormatAU::AudioPluginFormatAU() {
    impl = new Impl(Logger::global());
}

remidy::AudioPluginFormatAU::~AudioPluginFormatAU() {
    delete impl;
}

remidy::Logger* remidy::AudioPluginFormatAU::getLogger() {
    return impl->getLogger();
}

remidy::AudioPluginExtensibility<remidy::AudioPluginFormat> * remidy::AudioPluginFormatAU::getExtensibility() {
    // FIXME: implement
    throw std::runtime_error("getExtensibility() is not implemented yet.");
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
    std::unique_ptr<std::promise<InvokeResult>> promise{};

    // It is "fire and forget" async...
    auto result = std::async(std::launch::async, [this](PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) -> void {
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
                    callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv3>(this, component, instance), std::string{}});
                else
                    callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv2>(this, component, instance), std::string{}});
            }
            else
              callback(InvokeResult{nullptr, std::string("Failed to instantiate AudioUnit.")});
        });
    }, info, callback);
}


// AudioPluginInstanceAU

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(AudioPluginFormatAU *format, AudioComponent component, AudioComponentInstance instance) :
    format(format), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
    buses = inspectBuses();
}
remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
    AudioComponentInstanceDispose(instance);
}

remidy::StatusCode remidy::AudioPluginInstanceAU::configure(ConfigurationRequest& configuration) {
    OSStatus result;

    result = AudioUnitReset(instance, kAudioUnitScope_Global, 0);
    if (result) {
        format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to reset instance!?: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    sampleRate((double) configuration.sampleRate);

    AudioStreamBasicDescription stream{};
    stream.mSampleRate = configuration.sampleRate;
    stream.mFormatID = kAudioFormatLinearPCM;
    stream.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian | kLinearPCMFormatFlagIsPacked;
    stream.mBitsPerChannel = 8 * sizeof (float);
    stream.mFramesPerPacket = 1;
    // FIXME: retrieve from bus
    stream.mChannelsPerFrame = 2;
    stream.mBytesPerFrame = stream.mBytesPerPacket = sizeof (float);
    if (hasAudioInputs()) {
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                      &stream, sizeof(AudioStreamBasicDescription));
        if (result) {
            format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set input kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    if (hasAudioOutputs()) {
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                      &stream, sizeof(AudioStreamBasicDescription));
        if (result) {
            format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }

    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &configuration.offlineMode, sizeof(bool));
    if (result != 0) {
        this->format->getLogger()->logWarning("%s: configure() on AudioPluginInstanceAU failed to set offlineMode. Status: %d", name.c_str(), result);
    }

    UInt32 frameSize = (UInt32) configuration.bufferSizeInSamples;
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &frameSize, sizeof (frameSize));
    if (result) {
        format->getLogger()->logError("%s: AudioPluginInstanceAU::configure failed to set kAudioUnitProperty_MaximumFramesPerSlice: OSStatus %d", name.c_str(), result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }

    UInt32 size;
    AudioUnitGetProperty(instance, kAudioUnitProperty_InPlaceProcessing, kAudioUnitScope_Global, 0, &process_replacing, &size);
    // FIXME: this audio bus count is hacky and inaccurate.
    UInt32 nBuffers = hasAudioInputs() ? hasAudioOutputs() && !process_replacing ? 2 : 1 : 0;
    auData = std::make_unique<::AudioBufferList>();
    auData->mNumberBuffers = nBuffers;

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
/*
    int32_t dstBus = 0;
    uint32_t channelBufSize = process.frameCount() * sizeof(float);
    if (hasAudioInputs()) {
        for (int32_t bus = 0, n = process.audioInBusCount(); bus < n; bus++, dstBus++) {
            auto busBuf =process.audioIn(0);
            auData->mBuffers[dstBus].mNumberChannels = busBuf->channelCount();
            for (int32_t ch = 0; ch < busBuf->channelCount(); ch++) {
                // FIXME: might be 64bit float?
                if (ch == 0)
                    auData->mBuffers[dstBus].mData = busBuf->getFloatBufferForChannel(ch);
                else {
                    auto nextDst = (float*) auData->mBuffers[dstBus].mData + process.frameCount() * (ch);
                    auto nextSrc = busBuf->getFloatBufferForChannel(ch);
                    if (nextDst == nextSrc) {} // then no copy needed!
                    else
                        memcpy(nextDst, busBuf->getFloatBufferForChannel(ch), channelBufSize);
                }
            }
            auData->mBuffers[dstBus].mDataByteSize = channelBufSize * busBuf->channelCount();
        }
    }
    if (hasAudioOutputs() && !process_replacing) {
        for (int32_t bus = 0, n = process.audioOutBusCount(); bus < n; bus++, dstBus++) {
            auto busBuf =process.audioOut(0);
            auData->mBuffers[dstBus].mNumberChannels = busBuf->channelCount();
            for (int32_t ch = 0; ch < busBuf->channelCount(); ch++) {
                // FIXME: might be 64bit float?
                if (ch == 0)
                    auData->mBuffers[dstBus].mData = busBuf->getFloatBufferForChannel(ch);
                else {
                    auto nextPtr = (float*) auData->mBuffers[dstBus].mData + process.frameCount() * (ch);
                    if (nextPtr == busBuf->getFloatBufferForChannel(ch)) {} // then no copy needed!
                    else
                        memcpy(nextPtr, busBuf->getFloatBufferForChannel(ch), channelBufSize);
                }
            }
            auData->mBuffers[dstBus].mDataByteSize = channelBufSize * busBuf->channelCount();
        }
    }*/

    process_timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    auto status = AudioUnitRender(instance, nullptr, &process_timestamp, 0, process.frameCount(), auData.get());
    if (status != 0) {
        format->getLogger()->logError("%s: failed to process audio AudioPluginInstanceAU::process(). Status: %d", name.c_str(), status);
        return StatusCode::FAILED_TO_PROCESS;
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

    // FIXME: get numEventsIn and numEventsOut too.
    return ret;
}

// AudioPluginInstanceAUv2

remidy::StatusCode remidy::AudioPluginInstanceAUv2::sampleRate(double sampleRate) {
    UInt32* data;
    UInt32 size;

    if (audioUnitHasIO(instance, kAudioUnitScope_Input)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, sizeof(double));
        if (result != 0) {
            this->format->getLogger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set input sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    if (audioUnitHasIO(instance, kAudioUnitScope_Output)) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate, sizeof(double));
        if (result != 0) {
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
