#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
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
        StatusCode configure(int32_t sampleRate, bool offlineMode) override;

        StatusCode process(AudioProcessContext &process) override;

        StatusCode startProcessing() override;
        StatusCode stopProcessing() override;

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
    AVAudioUnitComponent* component;
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
    AVAudioUnitComponentManager* manager = [AVAudioUnitComponentManager sharedAudioUnitComponentManager];
    AudioComponentDescription desc{};
    auto list = [manager componentsMatchingDescription: desc];

    for (AVAudioUnitComponent* component in list) {
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

        NSString* nsName = [component name];
        const char* chars = [nsName UTF8String];
        auto name = std::string{chars};
        // LAMESPEC: it is quite hacky and lame expectation that the AudioComponent `name` always has `: ` ...
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

    /*
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
    }*/
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
            if (v3)
                callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv3>(this, component, instance), std::string{}});
            else
                callback(InvokeResult{std::make_unique<AudioPluginInstanceAUv2>(this, component, instance), std::string{}});
        });
    }, info, callback);
}


// AudioPluginInstanceAU

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(AudioPluginFormatAU *format, AudioComponent component, AudioComponentInstance instance) :
    format(format), component(component), instance(instance) {
    name = retrieveCFStringRelease([&](CFStringRef& cfName) -> void { AudioComponentCopyName(component, &cfName); });
}
remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
    AudioComponentInstanceDispose(instance);
}

remidy::StatusCode remidy::AudioPluginInstanceAU::configure(int32_t sampleRate, bool offlineMode) {
    OSStatus result;

    this->sampleRate((double) sampleRate);

    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offlineMode, sizeof(bool));
    if (result != 0) {
        this->format->getLogger()->logWarning("%s: configure() on AudioPluginInstanceAU failed to set offlineMode. Status: %d", name.c_str(), result);
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
    // FIXME: implement
    throw std::runtime_error("process() is not implemented yet.");
}

// AudioPluginInstanceAUv2

remidy::StatusCode remidy::AudioPluginInstanceAUv2::sampleRate(double sampleRate) {
    UInt32* data;
    UInt32 size;

    if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &data, &size) == 0 && *data > 0) {
        auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, sizeof(double));
        if (result != 0) {
            this->format->getLogger()->logError("%s: configure() on AudioPluginInstanceAUv2 failed to set input sampleRate. Status: %d", name.c_str(), result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }
    }
    if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &data, &size) == 0 && *data > 0) {
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
