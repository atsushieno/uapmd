#if __APPLE__

#include "PluginFormatAU.hpp"

void remidy::PluginInstanceAUv3::AudioBuses::inspectBuses() {
    @autoreleasepool {
        if (owner->audioUnit == nil)
            return;

        AUAudioUnit* au = owner->audioUnit;

        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        // Count input buses - use AUAudioUnitBusArray not NSArray
        AUAudioUnitBusArray* inputBusArray = [au inputBusses];
        busesInfo.numAudioIn = static_cast<uint32_t>([inputBusArray count]);

        // Count output buses
        AUAudioUnitBusArray* outputBusArray = [au outputBusses];
        busesInfo.numAudioOut = static_cast<uint32_t>([outputBusArray count]);

        // MIDI/Event support detection
        // Check if the AU supports MIDI input
        if ([au respondsToSelector:@selector(MIDIOutputNames)]) {
            NSArray* midiOutputs = [au MIDIOutputNames];
            busesInfo.numEventOut = static_cast<uint32_t>([midiOutputs count]);
        } else {
            busesInfo.numEventOut = 0;
        }

        // Most AUs that are instruments/MIDI effects accept MIDI input
        AudioComponentDescription desc = [au componentDescription];
        if (desc.componentType == kAudioUnitType_MusicDevice ||
            desc.componentType == kAudioUnitType_MusicEffect ||
            desc.componentType == kAudioUnitType_MIDIProcessor) {
            busesInfo.numEventIn = 1;
        } else {
            busesInfo.numEventIn = 0;
        }

        // Create bus definitions for input buses
        input_bus_defs.clear();
        audio_in_buses.clear();
        for (NSUInteger i = 0; i < [inputBusArray count]; i++) {
            AUAudioUnitBus* bus = [inputBusArray objectAtIndexedSubscript:i];
            AVAudioFormat* format = [bus format];

            NSString* busNameNS = [bus name];
            auto busName = busNameNS ? std::string([busNameNS UTF8String]) : "";
            auto channels = static_cast<uint32_t>([format channelCount]);

            // NOTE: some non-trivial replacement during AUv2->AUv3 migration
            // AudioBusDefinition constructor needs (name, role, layouts)
            // AudioChannelLayout needs (name, channelCount)
            std::vector<AudioChannelLayout> layouts;
            std::string layoutName = (channels == 1) ? "Mono" : "Stereo";
            layouts.emplace_back(layoutName, channels);

            AudioBusDefinition def(busName, AudioBusRole::Main, layouts);
            input_bus_defs.push_back(def);
            auto* busConfig = new AudioBusConfiguration(input_bus_defs.back());
            busConfig->channelLayout(layouts[0]);
            audio_in_buses.push_back(busConfig);
        }

        // Create bus definitions for output buses
        output_bus_defs.clear();
        audio_out_buses.clear();
        for (NSUInteger i = 0; i < [outputBusArray count]; i++) {
            AUAudioUnitBus* bus = [outputBusArray objectAtIndexedSubscript:i];
            AVAudioFormat* format = [bus format];

            NSString* busNameNS = [bus name];
            auto busName = busNameNS ? std::string([busNameNS UTF8String]) : "";
            auto channels = static_cast<uint32_t>([format channelCount]);

            // NOTE: some non-trivial replacement during AUv2->AUv3 migration
            std::vector<AudioChannelLayout> layouts;
            std::string layoutName = (channels == 1) ? "Mono" : "Stereo";
            layouts.emplace_back(layoutName, channels);

            AudioBusDefinition def(busName, AudioBusRole::Main, layouts);
            output_bus_defs.push_back(def);
            auto* busConfig = new AudioBusConfiguration(output_bus_defs.back());
            busConfig->channelLayout(layouts[0]);
            audio_out_buses.push_back(busConfig);
        }
    }
}

remidy::StatusCode remidy::PluginInstanceAUv3::AudioBuses::configure(ConfigurationRequest& configuration) {
    @autoreleasepool {
        if (owner->audioUnit == nil) {
            owner->logger()->logError("%s: AudioBuses::configure - audioUnit is nil", owner->name.c_str());
            return StatusCode::FAILED_TO_CONFIGURE;
        }

        AUAudioUnit* au = owner->audioUnit;
        NSError* error = nil;

        // Determine audio format type
        bool useDouble = configuration.dataType == AudioContentType::Float64;
        AVAudioCommonFormat commonFormat = useDouble ? AVAudioPCMFormatFloat64 : AVAudioPCMFormatFloat32;

        // Configure input buses
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AUAudioUnitBusArray* inputBusArray = [au inputBusses];
        for (NSUInteger i = 0; i < [inputBusArray count]; i++) {
            AUAudioUnitBus* bus = [inputBusArray objectAtIndexedSubscript:i];
            auto* busConfig = i < audio_in_buses.size() ? audio_in_buses[i] : nullptr;

            // Check if bus can be queried first (some plugins don't support all buses)
            AVAudioFormat* currentFormat = [bus format];
            if (currentFormat == nil) {
                owner->logger()->logWarning("%s: Input bus %lu has no format, skipping configuration",
                                           owner->name.c_str(), i);
                continue;
            }

            uint32_t channels = 2; // Default stereo
            if (i == 0 && configuration.mainInputChannels.has_value()) {
                channels = configuration.mainInputChannels.value();
            }

            AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:configuration.sampleRate
                                                                                    channels:channels];
            if (format == nil) {
                owner->logger()->logError("%s: Failed to create AVAudioFormat for input bus %lu",
                                        owner->name.c_str(), i);
                return StatusCode::FAILED_TO_CONFIGURE;
            }

            if (![bus setFormat:format error:&error]) {
                // Log warning but continue - some buses might not support configuration
                owner->logger()->logWarning("%s: Failed to set format on input bus %lu: %s (continuing)",
                                        owner->name.c_str(), i,
                                        [[error localizedDescription] UTF8String]);
                [format release];
                continue;
            }

            if (busConfig)
                [bus setEnabled:busConfig->enabled()];

            [format release];
        }

        // Configure output buses
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AUAudioUnitBusArray* outputBusArray = [au outputBusses];
        for (NSUInteger i = 0; i < [outputBusArray count]; i++) {
            AUAudioUnitBus* bus = [outputBusArray objectAtIndexedSubscript:i];
            auto* busConfig = i < audio_out_buses.size() ? audio_out_buses[i] : nullptr;

            // Check if bus can be queried first (some plugins don't support all buses)
            AVAudioFormat* currentFormat = [bus format];
            if (currentFormat == nil) {
                owner->logger()->logWarning("%s: Output bus %lu has no format, skipping configuration",
                                           owner->name.c_str(), i);
                continue;
            }

            uint32_t channels = 2; // Default stereo
            if (i == 0 && configuration.mainOutputChannels.has_value()) {
                channels = configuration.mainOutputChannels.value();
            }

            AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:configuration.sampleRate
                                                                                    channels:channels];
            if (format == nil) {
                owner->logger()->logError("%s: Failed to create AVAudioFormat for output bus %lu",
                                        owner->name.c_str(), i);
                return StatusCode::FAILED_TO_CONFIGURE;
            }

            if (![bus setFormat:format error:&error]) {
                // Log warning but continue - some buses might not support configuration
                owner->logger()->logWarning("%s: Failed to set format on output bus %lu: %s (continuing)",
                                        owner->name.c_str(), i,
                                        [[error localizedDescription] UTF8String]);
                [format release];
                continue;
            }

            if (busConfig)
                [bus setEnabled:busConfig->enabled()];

            [format release];
        }

        // Re-inspect buses after configuration
        inspectBuses();

        return StatusCode::OK;
    }
}

#endif
