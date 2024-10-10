#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace remidy {
    class AudioPluginFormatAU::Impl {
        Logger* logger;
    public:
        Impl(Logger* logger) : logger(logger) {}

        Logger* getLogger() { return logger; }
    };

    class AudioPluginInstanceAU : public AudioPluginInstance {
        AudioPluginFormatAU *format;
        AudioUnit instance;
    public:
        StatusCode configure(int32_t sampleRate, bool offlineMode) override;

        StatusCode process(AudioProcessContext &process) override;

        AudioPluginInstanceAU(AudioPluginFormatAU* format, AudioUnit instance);
        ~AudioPluginInstanceAU();
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
    AudioComponent component = nullptr;

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

        AudioComponentInstantiationOptions options = 0;
        AudioComponentInstantiate(component, options, ^(AudioComponentInstance instance, OSStatus status) {
            callback(InvokeResult{std::make_unique<AudioPluginInstanceAU>(this, instance), std::string{}});
        });
    }, info, callback);
}




remidy::StatusCode remidy::AudioPluginInstanceAU::configure(int32_t sampleRate, bool offlineMode) {
    double sampleRateDouble = (double) sampleRate;
    auto result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRateDouble, sizeof(double));
    if (result != 0) {
        this->format->getLogger()->logError("configure() on AudioPluginInstanceAU failed to set input sampleRate. Status: %d", result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRateDouble, sizeof(double));
    if (result != 0) {
        this->format->getLogger()->logError("configure() on AudioPluginInstanceAU failed to set output sampleRate. Status: %d", result);
        return StatusCode::FAILED_TO_CONFIGURE;
    }
    // it could be an invalid property. maybe just ignore that.
    result = AudioUnitSetProperty(instance, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offlineMode, sizeof(bool));
    if (result != 0) {
        this->format->getLogger()->logError("configure() on AudioPluginInstanceAU failed to set offlineMode. Status: %d", result);
    }

    return StatusCode::OK;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::process(AudioProcessContext &process) {
    // FIXME: implement
    throw std::runtime_error("process() is not implemented yet.");
}

remidy::AudioPluginInstanceAU::AudioPluginInstanceAU(AudioPluginFormatAU *format, AudioComponentInstance instance) :
    format(format), instance(instance) {
}
remidy::AudioPluginInstanceAU::~AudioPluginInstanceAU() {
    AudioComponentInstanceDispose(instance);
}

#endif
