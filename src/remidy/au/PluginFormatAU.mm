#if __APPLE__

#include <format>
#include "PluginFormatAU.hpp"

remidy::PluginFormatAU::PluginFormatAU() {
    impl = new Impl(this, Logger::global());
}

remidy::PluginFormatAU::~PluginFormatAU() {
    delete impl;
}

remidy::PluginExtensibility<remidy::PluginFormat> * remidy::PluginFormatAU::getExtensibility() {
    return impl->getExtensibility();
}

remidy::PluginScanning* remidy::PluginFormatAU::scanning() {
    return impl->scanning();
}

struct AUPluginEntry {
    AudioComponent component;
    UInt32 flags;
    std::string id;
    std::string name;
    std::string vendor;
};

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

void remidy::PluginFormatAU::createInstance(
        PluginCatalogEntry* info,
        PluginInstantiationOptions options,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
) {
    AudioComponentDescription desc{};
    std::istringstream id{info->pluginId()};
    id >> std::hex >> std::setw(2) >> desc.componentManufacturer >> desc.componentType >> desc.componentSubType;

    __block auto component = AudioComponentFindNext(nullptr, &desc);
    if (component == nullptr) {
        callback(nullptr, "The specified AudioUnit component was not found");
        return;
    }

    auto status = AudioComponentGetDescription(component, &desc);
    if (status) {
        callback(nullptr, "The specified AudioUnit component was not found");
        return;
    }
    bool v3 = (desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) > 0;
    AudioComponentInstantiationOptions auOptions = kAudioComponentInstantiation_LoadInProcess;

    __block auto cb = std::move(callback);
    AudioComponentInstantiate(component, auOptions, ^(AudioComponentInstance instance, OSStatus cbStatus) {
        if (cbStatus == noErr) {
            // FIXME: how should we acquire logger instance?
            auto logger = Logger::global();

            if (v3) {
                auto au = std::make_unique<AudioPluginInstanceAUv3>(this, options, logger, info, component, instance);
                cb(std::move(au), "");
            } else {
                auto au = std::make_unique<AudioPluginInstanceAUv2>(this, options, logger, info, component, instance);
                cb(std::move(au), "");
            }
        }
        else
          cb(nullptr, std::format("Failed to instantiate AudioUnit {}", info->displayName()));
    });
}

remidy::PluginFormatAU::Extensibility::Extensibility(PluginFormat &format) : PluginExtensibility(format) {
}

#endif
