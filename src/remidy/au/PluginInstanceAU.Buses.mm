#if __APPLE__

#include "PluginFormatAU.hpp"
#include <optional>
#include <choc/platform/choc_ObjectiveCHelpers.h>

namespace {
    // Helper to convert AudioChannelLayoutTag to channel count
    uint32_t getChannelCountFromLayoutTag(AudioChannelLayoutTag tag) {
        return AudioChannelLayoutTag_GetNumberOfChannels(tag);
    }

    // Helper to convert channel count to AudioChannelLayout
    remidy::AudioChannelLayout channelLayoutFromTag(AudioChannelLayoutTag tag) {
        uint32_t channelCount = getChannelCountFromLayoutTag(tag);

        // Map common tags to named layouts
        switch (tag) {
            case kAudioChannelLayoutTag_Mono:
                return remidy::AudioChannelLayout::mono();
            case kAudioChannelLayoutTag_Stereo:
            case kAudioChannelLayoutTag_StereoHeadphones:
            case kAudioChannelLayoutTag_MatrixStereo:
            case kAudioChannelLayoutTag_Binaural:
                return remidy::AudioChannelLayout::stereo();
            default:
                // For other layouts, use generic name based on channel count
                return remidy::AudioChannelLayout{
                    std::to_string(channelCount) + " channels",
                    channelCount
                };
        }
    }

    // Helper to convert AudioChannelLayout to AudioChannelLayoutTag
    AudioChannelLayoutTag channelLayoutToTag(const remidy::AudioChannelLayout& layout) {
        uint32_t channels = layout.channels();

        // Map common configurations to specific tags
        if (channels == 1) {
            return kAudioChannelLayoutTag_Mono;
        } else if (channels == 2) {
            return kAudioChannelLayoutTag_Stereo;
        } else {
            // Use DiscreteInOrder for arbitrary channel counts
            return kAudioChannelLayoutTag_DiscreteInOrder | static_cast<AudioChannelLayoutTag>(channels);
        }
    }
}

void remidy::PluginInstanceAU::AudioBuses::inspectBuses() {
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

        // AudioUnit has no concept of Main/Aux roles, treat first bus as Main
        AudioBusRole role = (i == 0) ? AudioBusRole::Main : AudioBusRole::Aux;
        AudioBusDefinition def{busName, role, {currentLayout}};
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

        // AudioUnit has no concept of Main/Aux roles, treat first bus as Main
        AudioBusRole role = (i == 0) ? AudioBusRole::Main : AudioBusRole::Aux;
        AudioBusDefinition def{busName, role, {currentLayout}};
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

remidy::StatusCode remidy::PluginInstanceAU::AudioBuses::configure(ConfigurationRequest& configuration) {
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
