#if __APPLE__

#include "PluginFormatAU.hpp"

void remidy::AudioPluginInstanceAU::AUAudioBuses::inspectBuses() {
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

    for (auto bus : input_buses)
        delete bus;
    for (auto bus : output_buses)
        delete bus;
    input_buses.clear();
    output_buses.clear();
    input_bus_defs.clear();
    output_bus_defs.clear();

    ::AudioChannelLayout auLayout;
    for (auto i = 0; i < ret.numAudioIn; i++) {
        auto busName = std::string{""};
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, i, &auLayout, &size) == noErr) {
            CFStringRef cfName{nullptr};
            if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementName, kAudioUnitScope_Input, i, &cfName, &size) == noErr && cfName != nullptr) {
                // FIXME: we need to fix something around here
                //busName = cfStringToString(cfName);
            }
        }
        AudioBusDefinition def{busName, AudioBusRole::Main}; // FIXME: correct bus type
        // FIXME: fill channel layouts
        // also use AudioChannelLayoutTag_GetNumberOfChannels(auLayout)
        input_bus_defs.emplace_back(def);
        input_buses.emplace_back(new AudioBusConfiguration(def));
    }
    for (auto i = 0; i < ret.numAudioOut; i++) {
        auto busName = std::string{""};
        if (AudioUnitGetProperty(instance, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, i, &auLayout, &size) == noErr) {
            CFStringRef cfName{nullptr};
            if (AudioUnitGetProperty(instance, kAudioUnitProperty_ElementName, kAudioUnitScope_Output, i, &cfName, &size) == noErr && cfName != nullptr) {
                // FIXME: we need to fix something around here
                //busName = cfStringToString(cfName);
            }
        }
        AudioBusDefinition def{busName, AudioBusRole::Main}; // FIXME: correct bus type
        // FIXME: fill channel layouts
        // also use AudioChannelLayoutTag_GetNumberOfChannels(auLayout)
        output_bus_defs.emplace_back(def);
        output_buses.emplace_back(new AudioBusConfiguration(def));
    }

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

    buses = ret;
}

remidy::StatusCode remidy::AudioPluginInstanceAU::AUAudioBuses::configure(ConfigurationRequest& configuration) {
    auto& component = owner->component;
    auto& instance = owner->instance;
    auto logger = owner->logger();
    auto& name = owner->name;

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
            // FIXME: retrieve from bus
            stream.mChannelsPerFrame = 2;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                logger->logError("%s AudioPluginInstanceAU::configure failed to set input kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                return StatusCode::FAILED_TO_CONFIGURE;
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
            // FIXME: retrieve from bus
            stream.mChannelsPerFrame = 2;
            result = AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, i,
                                          &stream, sizeof(AudioStreamBasicDescription));
            if (result) {
                logger->logError("%s: AudioPluginInstanceAU::configure failed to set output kAudioUnitProperty_StreamFormat: OSStatus %d", name.c_str(), result);
                return StatusCode::FAILED_TO_CONFIGURE;
            }
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

    return StatusCode::OK;
}

#endif
