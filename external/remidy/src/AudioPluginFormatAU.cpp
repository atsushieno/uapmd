#if __APPLE__

#include <iostream>
#include <sstream>

#include "remidy.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

namespace remidy {
    class AudioPluginFormatAU::Impl {
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

std::vector<std::string> ret{};
std::vector<std::string>& remidy::AudioPluginFormatAU::getDefaultSearchPaths() {
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
        PluginCatalogEntry entry{};
        entry.pluginId(plugin.id);
        entry.setMetadataProperty(PluginCatalogEntry::DisplayName, plugin.name);
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

inline remidy::AudioPluginInstance * remidy::AudioPluginFormatAU::createInstance(PluginCatalogEntry *uniqueId) {
    // FIXME: implement
    throw std::runtime_error("createInstance() is not implemented yet.");
}

#endif
