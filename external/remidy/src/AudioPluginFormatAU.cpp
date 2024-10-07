#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace remidy {
    class AudioPluginFormatAU::Impl {
    };

    class AudioPluginInstanceAU : public AudioPluginInstance {
        AudioPluginFormatAU *format;
        AudioComponentInstance instance;
    public:
        StatusCode configure(int32_t sampleRate) override;

        StatusCode process(AudioProcessContext &process) override;

        AudioPluginInstanceAU(AudioPluginFormatAU* format, AudioComponentInstance instance);
        ~AudioPluginInstanceAU();
    };
}

remidy::AudioPluginFormatAU::AudioPluginFormatAU() {
    impl = new Impl();
}

remidy::AudioPluginFormatAU::~AudioPluginFormatAU() {
    delete impl;
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

remidy::PluginCatalog remidy::AudioPluginFormatAU::scanAllAvailablePlugins() {
    remidy::PluginCatalog ret{};

    for (auto& plugin : scanAllAvailableAUPlugins()) {
        std::cerr << "Found: [" << plugin.id << " (flags: " << plugin.flags <<  ")] " << plugin.vendor << ": " << plugin.name << std::endl;
        auto entry = std::make_unique<PluginCatalogEntry>();
        entry->pluginId(plugin.id);
        entry->setMetadataProperty(PluginCatalogEntry::DisplayName, plugin.name);
        entry->setMetadataProperty(PluginCatalogEntry::VendorName, plugin.vendor);
        ret.add(std::move(entry));
    }
    return ret;
}

std::string remidy::AudioPluginFormatAU::savePluginInformation(PluginCatalogEntry *identifier) {
    // FIXME: implement
    throw std::runtime_error("savePluginInformation() is not implemented yet.");
}

std::string remidy::AudioPluginFormatAU::savePluginInformation(AudioPluginInstance *instance) {
    // FIXME: implement
    throw std::runtime_error("savePluginInformation() is not implemented yet.");
}

std::unique_ptr<remidy::PluginCatalogEntry> remidy::AudioPluginFormatAU::restorePluginInformation(std::string &data) {
    // FIXME: implement
    throw std::runtime_error("restorePluginInformation() is not implemented yet.");
}

void remidy::AudioPluginFormatAU::createInstance(PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) {
    std::unique_ptr<std::promise<InvokeResult>> promise{};

    std::async(std::launch::async, [this](PluginCatalogEntry *info, std::function<void(InvokeResult)> callback) -> void {
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




remidy::StatusCode remidy::AudioPluginInstanceAU::configure(int32_t sampleRate) {
    // FIXME: implement
    throw std::runtime_error("configure() is not implemented yet.");
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
