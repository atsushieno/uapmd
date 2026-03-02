#if __APPLE__

#include "PluginFormatAU.hpp"
#include <optional>
#include <choc/platform/choc_ObjectiveCHelpers.h>

namespace {
    // Helper to convert AudioChannelLayoutTag to channel count
    uint32_t getChannelCountFromLayoutTag(AudioChannelLayoutTag tag) {
        return AudioChannelLayoutTag_GetNumberOfChannels(tag);
    }

    // Comprehensive mapping table from AudioChannelLayoutTag to named layouts
    struct AULayoutMapping {
        AudioChannelLayoutTag tag;
        const char* name;
        uint32_t channels;
    };

    // Based on CoreAudioTypes.h AudioChannelLayoutTag definitions
    constexpr AULayoutMapping auLayoutMappings[] = {
        // Standard layouts
        { kAudioChannelLayoutTag_Mono,                  "Mono",                 1 },
        { kAudioChannelLayoutTag_Stereo,                "Stereo",               2 },
        { kAudioChannelLayoutTag_StereoHeadphones,      "Stereo Headphones",    2 },
        { kAudioChannelLayoutTag_MatrixStereo,          "Matrix Stereo",        2 },
        { kAudioChannelLayoutTag_MidSide,               "Mid/Side",             2 },
        { kAudioChannelLayoutTag_XY,                    "XY",                   2 },
        { kAudioChannelLayoutTag_Binaural,              "Binaural",             2 },

        // Ambisonics
        { kAudioChannelLayoutTag_Ambisonic_B_Format,    "Ambisonic B-Format",   4 },

        // Surround - Quadraphonic and up
        { kAudioChannelLayoutTag_Quadraphonic,          "Quadraphonic",         4 },
        { kAudioChannelLayoutTag_Pentagonal,            "Pentagonal",           5 },
        { kAudioChannelLayoutTag_Hexagonal,             "Hexagonal",            6 },
        { kAudioChannelLayoutTag_Octagonal,             "Octagonal",            8 },
        { kAudioChannelLayoutTag_Cube,                  "Cube",                 8 },

        // MPEG/ITU formats
        { kAudioChannelLayoutTag_MPEG_3_0_A,            "3.0 (L R C)",          3 },
        { kAudioChannelLayoutTag_MPEG_3_0_B,            "3.0 (C L R)",          3 },
        { kAudioChannelLayoutTag_MPEG_4_0_A,            "4.0 (L R C Cs)",       4 },
        { kAudioChannelLayoutTag_MPEG_4_0_B,            "4.0 (C L R Cs)",       4 },
        { kAudioChannelLayoutTag_MPEG_5_0_A,            "5.0 (L R C Ls Rs)",    5 },
        { kAudioChannelLayoutTag_MPEG_5_0_B,            "5.0 (L R Ls Rs C)",    5 },
        { kAudioChannelLayoutTag_MPEG_5_0_C,            "5.0 (L C R Ls Rs)",    5 },
        { kAudioChannelLayoutTag_MPEG_5_0_D,            "5.0 (C L R Ls Rs)",    5 },
        { kAudioChannelLayoutTag_MPEG_5_1_A,            "5.1 (L R C LFE Ls Rs)", 6 },
        { kAudioChannelLayoutTag_MPEG_5_1_B,            "5.1 (L R Ls Rs C LFE)", 6 },
        { kAudioChannelLayoutTag_MPEG_5_1_C,            "5.1 (L C R Ls Rs LFE)", 6 },
        { kAudioChannelLayoutTag_MPEG_5_1_D,            "5.1 (C L R Ls Rs LFE)", 6 },
        { kAudioChannelLayoutTag_MPEG_6_1_A,            "6.1",                  7 },
        { kAudioChannelLayoutTag_MPEG_7_1_A,            "7.1 (SDDS A)",         8 },
        { kAudioChannelLayoutTag_MPEG_7_1_B,            "7.1 (SDDS B)",         8 },
        { kAudioChannelLayoutTag_MPEG_7_1_C,            "7.1",                  8 },
        { kAudioChannelLayoutTag_Emagic_Default_7_1,    "7.1 (Emagic)",         8 },
        { kAudioChannelLayoutTag_SMPTE_DTV,             "SMPTE DTV",            8 },

        // ITU variants
        { kAudioChannelLayoutTag_ITU_2_1,               "ITU 2.1",              3 },
        { kAudioChannelLayoutTag_ITU_2_2,               "ITU 2.2",              4 },

        // DVD formats
        { kAudioChannelLayoutTag_DVD_4,                 "DVD 4 (L R LFE)",      3 },
        { kAudioChannelLayoutTag_DVD_5,                 "DVD 5 (L R LFE Cs)",   4 },
        { kAudioChannelLayoutTag_DVD_6,                 "DVD 6 (L R LFE Ls Rs)", 5 },
        { kAudioChannelLayoutTag_DVD_10,                "DVD 10",               4 },
        { kAudioChannelLayoutTag_DVD_11,                "DVD 11",               5 },
        { kAudioChannelLayoutTag_DVD_18,                "DVD 18",               5 },

        // AudioUnit specific
        { kAudioChannelLayoutTag_AudioUnit_6_0,         "AU 6.0",               6 },
        { kAudioChannelLayoutTag_AudioUnit_7_0,         "AU 7.0",               7 },
        { kAudioChannelLayoutTag_AudioUnit_7_0_Front,   "AU 7.0 Front",         7 },

        // AAC formats
        { kAudioChannelLayoutTag_AAC_6_0,               "AAC 6.0",              6 },
        { kAudioChannelLayoutTag_AAC_6_1,               "AAC 6.1",              7 },
        { kAudioChannelLayoutTag_AAC_7_0,               "AAC 7.0",              7 },
        { kAudioChannelLayoutTag_AAC_7_1_B,             "AAC 7.1 B",            8 },
        { kAudioChannelLayoutTag_AAC_7_1_C,             "AAC 7.1 C",            8 },
        { kAudioChannelLayoutTag_AAC_Octagonal,         "AAC Octagonal",        8 },

        // TMH (22.2)
        { kAudioChannelLayoutTag_TMH_10_2_std,          "TMH 10.2",             16 },
        { kAudioChannelLayoutTag_TMH_10_2_full,         "TMH 10.2 Full",        21 },

        // AC3/EAC3
        { kAudioChannelLayoutTag_AC3_1_0_1,             "AC3 1.0.1",            2 },
        { kAudioChannelLayoutTag_AC3_3_0,               "AC3 3.0",              3 },
        { kAudioChannelLayoutTag_AC3_3_1,               "AC3 3.1",              4 },
        { kAudioChannelLayoutTag_AC3_3_0_1,             "AC3 3.0.1",            4 },
        { kAudioChannelLayoutTag_AC3_2_1_1,             "AC3 2.1.1",            4 },
        { kAudioChannelLayoutTag_AC3_3_1_1,             "AC3 3.1.1",            5 },
        { kAudioChannelLayoutTag_EAC_6_0_A,             "EAC 6.0",              6 },
        { kAudioChannelLayoutTag_EAC_7_0_A,             "EAC 7.0",              7 },
        { kAudioChannelLayoutTag_EAC3_6_1_A,            "EAC3 6.1",             7 },

        // Atmos formats
        { kAudioChannelLayoutTag_Atmos_5_1_2,           "Atmos 5.1.2",          8 },
        { kAudioChannelLayoutTag_Atmos_5_1_4,           "Atmos 5.1.4",          10 },
        { kAudioChannelLayoutTag_Atmos_7_1_2,           "Atmos 7.1.2",          10 },
        { kAudioChannelLayoutTag_Atmos_7_1_4,           "Atmos 7.1.4",          12 },
        { kAudioChannelLayoutTag_Atmos_9_1_6,           "Atmos 9.1.6",          16 },
    };

    remidy::AudioChannelLayout layoutForChannels(uint32_t channels) {
        switch (channels) {
            case 0:
                return remidy::AudioChannelLayout{"", 0};
            case 1:
                return remidy::AudioChannelLayout{"Mono", 1};
            case 2:
                return remidy::AudioChannelLayout{"Stereo", 2};
            default:
                return remidy::AudioChannelLayout{"", channels};
        }
    }

    // Helper to convert AudioChannelLayoutTag to AudioChannelLayout with comprehensive mapping
    remidy::AudioChannelLayout channelLayoutFromTag(AudioChannelLayoutTag tag) {
        // Try to find exact match in mapping table
        for (const auto& mapping : auLayoutMappings) {
            if (mapping.tag == tag) {
                return remidy::AudioChannelLayout{mapping.name, mapping.channels};
            }
        }

        // Fallback: get channel count and return generic layout
        uint32_t channelCount = getChannelCountFromLayoutTag(tag);
        return layoutForChannels(channelCount);
    }

    // Helper to convert AudioChannelLayout to AudioChannelLayoutTag with comprehensive mapping
    AudioChannelLayoutTag channelLayoutToTag(const remidy::AudioChannelLayout& layout) {
        uint32_t channels = layout.channels();

        // Handle empty layout
        if (channels == 0)
            return kAudioChannelLayoutTag_Unknown;

        // Try to find matching tag by name first (if name is specified)
        const auto& srcName = const_cast<remidy::AudioChannelLayout&>(layout).name();
        if (!srcName.empty()) {
            for (const auto& mapping : auLayoutMappings) {
                if (mapping.channels == channels && srcName == mapping.name) {
                    return mapping.tag;
                }
            }
        }

        // Fallback to standard layouts based on channel count
        switch (channels) {
            case 1: return kAudioChannelLayoutTag_Mono;
            case 2: return kAudioChannelLayoutTag_Stereo;
            case 3: return kAudioChannelLayoutTag_MPEG_3_0_A;     // L R C
            case 4: return kAudioChannelLayoutTag_Quadraphonic;   // L R Ls Rs
            case 5: return kAudioChannelLayoutTag_MPEG_5_0_A;     // L R C Ls Rs
            case 6: return kAudioChannelLayoutTag_MPEG_5_1_A;     // L R C LFE Ls Rs
            case 7: return kAudioChannelLayoutTag_AudioUnit_7_0;  // L R Ls Rs C Rls Rrs
            case 8: return kAudioChannelLayoutTag_MPEG_7_1_C;     // L R C LFE Ls Rs Rls Rrs
            default:
                // Use DiscreteInOrder for arbitrary channel counts
                return kAudioChannelLayoutTag_DiscreteInOrder | static_cast<AudioChannelLayoutTag>(channels);
        }
    }
}

void remidy::PluginInstanceAUv2::AudioBuses::inspectBuses() {
    auto impl = [&] {
    auto component = owner->component;
    auto instance = owner->instance;
    auto logger = owner->logger();
    auto& name = owner->name;

    OSStatus result;
    BusSearchResult ret{};
    ::AudioChannelLayout layout{};
    UInt32 count{};
    UInt32 size = sizeof(UInt32);
    result = AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &count, &size);
    if (result)
        logger->logWarning("%s: failed to retrieve input kAudioUnitProperty_ElementCount. Status: %d", name.c_str(), result);
    else
        ret.numAudioIn = count;
    result = AudioUnitGetProperty(instance, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &count, &size);
    if (result)
        logger->logWarning("%s: failed to retrieve output kAudioUnitProperty_ElementCount. Status: %d", name.c_str(), result);
    else
        ret.numAudioOut = count;

    // FIXME: we need to fill input_buses and output_buses here.

    for (auto bus : audio_in_buses)
        delete bus;
    for (auto bus : audio_out_buses)
        delete bus;
    audio_in_buses.clear();
    audio_out_buses.clear();
    input_bus_defs.clear();
    output_bus_defs.clear();

    // Query input buses with actual channel information from StreamFormat
    AudioStreamBasicDescription streamFormat{};
    for (auto i = 0; i < ret.numAudioIn; i++) {
        auto busName = std::string{""};
        UInt32 streamFormatSize = sizeof(AudioStreamBasicDescription);

        // Get actual channel count from StreamFormat (more reliable than AudioChannelLayout)
        AudioChannelLayout currentLayout = AudioChannelLayout::stereo();
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i, &streamFormat, &streamFormatSize) == noErr) {
            currentLayout = channelLayoutFromTag(
                kAudioChannelLayoutTag_DiscreteInOrder | static_cast<AudioChannelLayoutTag>(streamFormat.mChannelsPerFrame)
            );
        }

        // Try to get bus name using choc helpers
        CFStringRef cfName{nullptr};
        UInt32 nameSize = sizeof(CFStringRef);
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementName, kAudioUnitScope_Input, i, &cfName, &nameSize) == noErr && cfName != nullptr) {
            busName = choc::objc::getString((__bridge id) cfName);
            CFRelease(cfName);
        }

        // Query supported channel layouts for this bus
        std::vector<AudioChannelLayout> supportedLayouts;
        UInt32 layoutSize = 0;
        result = AudioUnitGetPropertyInfo(instance, kAudioUnitProperty_SupportedChannelLayoutTags, kAudioUnitScope_Input, i, &layoutSize, nullptr);
        if (result == noErr && layoutSize > 0) {
            auto numTags = layoutSize / sizeof(AudioChannelLayoutTag);
            std::vector<AudioChannelLayoutTag> tags(numTags);
            result = AudioUnitGetProperty(instance, kAudioUnitProperty_SupportedChannelLayoutTags, kAudioUnitScope_Input, i, tags.data(), &layoutSize);
            if (result == noErr) {
                for (const auto& tag : tags) {
                    supportedLayouts.push_back(channelLayoutFromTag(tag));
                }
            }
        }
        // If no supported layouts were found, use the current layout as the only supported one
        if (supportedLayouts.empty()) {
            supportedLayouts.push_back(currentLayout);
        }

        // AudioUnit has no concept of Main/Aux roles, treat first bus as Main
        AudioBusRole role = (i == 0) ? AudioBusRole::Main : AudioBusRole::Aux;
        AudioBusDefinition def{busName, role, supportedLayouts};
        input_bus_defs.emplace_back(def);
        auto* busConfig = new AudioBusConfiguration(def);
        busConfig->channelLayout(currentLayout);
        audio_in_buses.emplace_back(busConfig);
    }

    // Query output buses with actual channel information from StreamFormat
    for (auto i = 0; i < ret.numAudioOut; i++) {
        auto busName = std::string{""};
        UInt32 streamFormatSize = sizeof(AudioStreamBasicDescription);

        // Get actual channel count from StreamFormat (more reliable than AudioChannelLayout)
        AudioChannelLayout currentLayout = AudioChannelLayout::stereo();
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i, &streamFormat, &streamFormatSize) == noErr) {
            currentLayout = channelLayoutFromTag(
                kAudioChannelLayoutTag_DiscreteInOrder | static_cast<AudioChannelLayoutTag>(streamFormat.mChannelsPerFrame)
            );
        }

        // Try to get bus name using choc helpers
        CFStringRef cfName{nullptr};
        UInt32 nameSize = sizeof(CFStringRef);
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementName, kAudioUnitScope_Output, i, &cfName, &nameSize) == noErr && cfName != nullptr) {
            busName = choc::objc::getString((__bridge id) cfName);
            CFRelease(cfName);
        }

        // Query supported channel layouts for this bus
        std::vector<AudioChannelLayout> supportedLayouts;
        UInt32 layoutSize = 0;
        result = AudioUnitGetPropertyInfo(instance, kAudioUnitProperty_SupportedChannelLayoutTags, kAudioUnitScope_Output, i, &layoutSize, nullptr);
        if (result == noErr && layoutSize > 0) {
            auto numTags = layoutSize / sizeof(AudioChannelLayoutTag);
            std::vector<AudioChannelLayoutTag> tags(numTags);
            result = AudioUnitGetProperty(instance, kAudioUnitProperty_SupportedChannelLayoutTags, kAudioUnitScope_Output, i, tags.data(), &layoutSize);
            if (result == noErr) {
                for (const auto& tag : tags) {
                    supportedLayouts.push_back(channelLayoutFromTag(tag));
                }
            }
        }
        // If no supported layouts were found, use the current layout as the only supported one
        if (supportedLayouts.empty()) {
            supportedLayouts.push_back(currentLayout);
        }

        // AudioUnit has no concept of Main/Aux roles, treat first bus as Main
        AudioBusRole role = (i == 0) ? AudioBusRole::Main : AudioBusRole::Aux;
        AudioBusDefinition def{busName, role, supportedLayouts};
        output_bus_defs.emplace_back(def);
        auto* busConfig = new AudioBusConfiguration(def);
        busConfig->channelLayout(currentLayout);
        audio_out_buses.emplace_back(busConfig);
    }

    AudioComponentDescription desc;
    AudioComponentGetDescription(component, &desc);
    switch (desc.componentType) {
        case kAudioUnitType_MusicDevice:
        case kAudioUnitType_MusicEffect:
        case kAudioUnitType_MIDIProcessor:
            ret.numEventIn = 1;
    }
    Boolean writable;
    auto status = AudioUnitGetPropertyInfo(instance, kAudioUnitProperty_MIDIOutputCallbackInfo, kAudioUnitScope_Global, 0, nullptr, &writable);
    if (status == noErr && writable)
        ret.numEventOut = 1;

    busesInfo = ret;

    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::InstanceControl)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();
}

remidy::StatusCode remidy::PluginInstanceAUv2::AudioBuses::configure(ConfigurationRequest& configuration) {
    remidy::StatusCode ret;
    auto impl = [&] {
    auto& component = owner->component;
    auto& instance = owner->instance;
    auto logger = owner->logger();
    auto& name = owner->name;

    auto applyRequestedChannels = [](std::vector<remidy::AudioBusConfiguration*>& buses, int32_t busIndex, const std::optional<uint32_t>& requested) {
        if (!requested.has_value())
            return;
        if (busIndex < 0 || static_cast<size_t>(busIndex) >= buses.size())
            return;
        auto bus = buses[static_cast<size_t>(busIndex)];
        auto channels = requested.value();
        bus->enabled(channels > 0);
        if (channels == 0)
            return;
        remidy::AudioChannelLayout layout{"", channels};
        if (channels == 1)
            layout = remidy::AudioChannelLayout{"Mono", 1};
        else if (channels == 2)
            layout = remidy::AudioChannelLayout{"Stereo", 2};
        if (bus->channelLayout(layout) != remidy::StatusCode::OK)
            bus->channelLayout() = layout;
    };

    applyRequestedChannels(audio_in_buses, mainInputBusIndex(), configuration.mainInputChannels);
    applyRequestedChannels(audio_out_buses, mainOutputBusIndex(), configuration.mainOutputChannels);

    OSStatus result;
    UInt32 size; // unused field for AudioUnitGetProperty

    UInt32 sampleSize = configuration.dataType == AudioContentType::Float64 ? sizeof(double) : sizeof(float);
    AudioStreamBasicDescription stream{};
    for (size_t i = 0, n = audioInputBuses().size(); i < n; i++) {
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
            auto channels = audioInputBuses()[i]->channelLayout().channels();
            stream.mChannelsPerFrame = channels;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                logger->logError("%s PluginInstanceAU::configure failed to set input kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                ret = StatusCode::FAILED_TO_CONFIGURE;
                return;
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
    for (size_t i = 0, n = audioOutputBuses().size(); i < n; i++) {
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
            auto channels = audioOutputBuses()[i]->channelLayout().channels();
            stream.mChannelsPerFrame = channels;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                logger->logError("%s: PluginInstanceAU::configure failed to set output kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                ret = StatusCode::FAILED_TO_CONFIGURE;
                return;
            }
        }

        /*
        ::AudioChannelLayout auLayout{};
        // FIXME: retrieve from bus
        auLayout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        result = AudioUnitSetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, i, &auLayout, sizeof(::AudioChannelLayout));
        if (result) {
            format->getLogger()->logError("%s AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_AudioChannelLayout: OSStatus %d", name.c_str(), result);
            ret = StatusCode::FAILED_TO_CONFIGURE;
            return ret;
        }*/
    }

    ret = StatusCode::OK;

    };
    if (owner->requiresUIThreadOn() & PluginUIThreadRequirement::State)
        EventLoop::runTaskOnMainThread(impl);
    else
        impl();

    return ret;
}

#endif
