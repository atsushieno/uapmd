#if __APPLE__

#include <format>
#include <CoreFoundation/CoreFoundation.h>
#include "PluginFormatAU.hpp"
#include "remidy/priv/plugin-format-au.hpp"
#include "AUv2Helper.hpp"

std::unique_ptr<remidy::PluginFormatAU> remidy::PluginFormatAU::create() {
    return std::make_unique<remidy::PluginFormatAUImpl>();
}

namespace {

std::string trimWhitespaceCopy(const std::string& value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string deriveVendorFromComponent(AudioComponent component) {
    CFStringRef nameCF = nullptr;
    if (AudioComponentCopyName(component, &nameCF) != noErr || nameCF == nullptr)
        return {};

    std::string componentName = cfStringToString(nameCF);
    CFRelease(nameCF);

    auto colonPos = componentName.find(':');
    if (colonPos == std::string::npos)
        return {};

    return trimWhitespaceCopy(componentName.substr(0, colonPos));
}

std::string manufacturerCodeToString(UInt32 code) {
    char buffer[5];
    buffer[0] = static_cast<char>((code >> 24) & 0xFF);
    buffer[1] = static_cast<char>((code >> 16) & 0xFF);
    buffer[2] = static_cast<char>((code >> 8) & 0xFF);
    buffer[3] = static_cast<char>(code & 0xFF);
    buffer[4] = '\0';

    auto trimmed = trimWhitespaceCopy(std::string(buffer, 4));
    if (!trimmed.empty())
        return trimmed;

    return std::format("{:08X}", code);
}

} // namespace

struct AUPluginEntry {
    AudioComponent component;
    AudioComponentDescription desc;
    UInt32 flags;
    std::string id;
    std::string name;
    std::string vendor;
    bool isV3;
};

std::vector<AUPluginEntry> scanAllAvailableAUPluginsV3() {
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

        CFStringRef nameCF = nullptr;
        AudioComponentCopyName(component, &nameCF);
        std::string name = nameCF ? cfStringToString(nameCF) : "";
        if (nameCF)
            CFRelease(nameCF);

        auto firstColon = name.find_first_of(':');
        auto vendor = firstColon != std::string::npos ? name.substr(0, firstColon) : "";
        auto pluginName = firstColon != std::string::npos ? name.substr(firstColon + 2) : name;

        std::ostringstream id{};
        id << std::hex << desc.componentManufacturer << " " << desc.componentType << " " << desc.componentSubType;

        bool isV3 = (desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;

        ret.emplace_back(AUPluginEntry{
            component,
            desc,
            desc.componentFlags,
            id.str(),
            pluginName,
            vendor,
            isV3
        });
    }
    return ret;
}

std::vector<std::unique_ptr<remidy::PluginCatalogEntry>> remidy::PluginScannerAU::scanAllAvailablePlugins() {
    std::vector<std::unique_ptr<PluginCatalogEntry>> ret{};
    auto plugins = scanAllAvailableAUPluginsV3();
    ret.reserve(plugins.size());

    for (auto& plugin : plugins) {
        auto entry = std::make_unique<PluginCatalogEntry>();
        static std::string format{"AU"};
        entry->format(format);
        entry->pluginId(plugin.id);
        entry->displayName(plugin.name);
        auto vendor = deriveVendorFromComponent(plugin.component);
        if (vendor.empty())
            vendor = manufacturerCodeToString(plugin.desc.componentManufacturer);
        if (!vendor.empty())
            entry->vendorName(vendor);
        // Store whether it's native v3 or v2-wrapped in metadata if needed
        // For now, we handle both through AUAudioUnit API

        ret.emplace_back(std::move(entry));
    }
    return ret;
}

void remidy::PluginFormatAUImpl::createInstance(
        PluginCatalogEntry* info,
        PluginInstantiationOptions options,
        std::function<void(std::unique_ptr<PluginInstance> instance, std::string error)> callback
) {
    auto implFunc = [&] {
        AudioComponentDescription desc{};
        std::istringstream id{info->pluginId()};
        id >> std::hex >> desc.componentManufacturer >> desc.componentType >> desc.componentSubType;

        __block auto component = AudioComponentFindNext(nullptr, &desc);
        if (component == nullptr) {
            callback(nullptr, "The specified AudioUnit component was not found");
            return;
        }

        auto status = AudioComponentGetDescription(component, &desc);
        if (status) {
            callback(nullptr, "Failed to get AudioUnit component description");
            return;
        }

        bool isV3 = (desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0;
        auto cb = std::move(callback);
        __block auto self = this;
        __block auto formatLogger = logger;

        if (!isV3) {
            AudioComponentInstantiationOptions auOptions = kAudioComponentInstantiation_LoadInProcess;
            __block PluginInstance* createdInstanceRaw = nullptr;
            __block std::string instantiationError;

            AudioComponentInstantiate(component, auOptions, ^(AudioComponentInstance instance, OSStatus cbStatus) {
                if (cbStatus == noErr) {
                    createdInstanceRaw = new PluginInstanceAUv2(self, options, formatLogger, info, component, instance);
                } else {
                    instantiationError = std::format("Failed to instantiate AudioUnit {} (status {})",
                                                     info->displayName(), cbStatus);
                }
            });

            if (createdInstanceRaw) {
                cb(std::unique_ptr<PluginInstance>(createdInstanceRaw), "");
            } else {
                if (instantiationError.empty())
                    instantiationError = "AudioUnit instantiation did not complete";
                cb(nullptr, instantiationError);
            }
            return;
        }

        // Use AUAudioUnit API for native AUv3s
        AudioComponentInstantiationOptions auOptions = kAudioComponentInstantiation_LoadInProcess;

        __block bool instantiationCompleted = false;
        __block PluginInstance* createdInstanceRaw = nullptr;
        __block std::string instantiationError;

        [AVAudioUnit instantiateWithComponentDescription:desc
                                                 options:auOptions
                                       completionHandler:^(AVAudioUnit * _Nullable avAudioUnit, NSError * _Nullable error) {
            if (error != nil) {
                NSString *errorMsg = [error localizedDescription];
                instantiationError = std::format("Failed to instantiate AUAudioUnit: {}", [errorMsg UTF8String]);
                instantiationCompleted = true;
                return;
            }

            if (avAudioUnit == nil) {
                instantiationError = std::format("Failed to instantiate AUAudioUnit {}", info->displayName());
                instantiationCompleted = true;
                return;
            }

            AUAudioUnit* audioUnit = [avAudioUnit AUAudioUnit];
            if (audioUnit == nil) {
                instantiationError = std::format("Failed to obtain AUAudioUnit from AVAudioUnit {}", info->displayName());
                instantiationCompleted = true;
                return;
            }

            // Retain both Objective-C objects since we're passing them to C++
            [avAudioUnit retain];
            [audioUnit retain];

            createdInstanceRaw = new PluginInstanceAUv3(self, options, formatLogger, info, avAudioUnit, audioUnit);
            instantiationCompleted = true;
        }];

        while (!instantiationCompleted)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);

        if (createdInstanceRaw) {
            cb(std::unique_ptr<PluginInstance>(createdInstanceRaw), "");
        } else {
            if (instantiationError.empty())
                instantiationError = "AUAudioUnit instantiation did not complete";
            cb(nullptr, instantiationError);
        }
    };

    // AUAudioUnit instantiation relies on the main run loop in many AUv3s (e.g. Mela).
    // Always run this on the UI thread so the completion handler is invoked.
    EventLoop::runTaskOnMainThread(implFunc);
}

remidy::PluginFormatAU::Extensibility::Extensibility(PluginFormat &format) : PluginExtensibility(format) {
}

#endif
