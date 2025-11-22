#include "PluginFormatVST3.hpp"
#include <optional>

namespace {
    // Helper to count bits in SpeakerArrangement
    uint32_t countChannels(SpeakerArrangement arr) {
        uint32_t count = 0;
        for (int32_t i = 0; i < 64; i++)
            if (arr & (1ULL << i))
                count++;
        return count;
    }

    // Comprehensive mapping table from SpeakerArrangement to AudioChannelLayout
    struct ArrangementMapping {
        SpeakerArrangement arrangement;
        const char* name;
        uint32_t channels;
    };

    // Based on VST3 SDK SpeakerArrangements
    constexpr ArrangementMapping arrangementMappings[] = {
        // Standard layouts
        { SpeakerArr::kEmpty,       "",          0 },
        { SpeakerArr::kMono,        "Mono",      1 },
        { SpeakerArr::kStereo,      "Stereo",    2 },
        { SpeakerArr::kStereoSurround, "Stereo Surround", 2 },
        { SpeakerArr::kStereoCenter, "Stereo Center", 3 },
        { SpeakerArr::kStereoSide,  "Stereo Side", 2 },
        { SpeakerArr::kStereoCLfe,  "Stereo C LFE", 3 },

        // Surround formats
        { SpeakerArr::k30Cine,      "3.0 Cine",   3 },
        { SpeakerArr::k30Music,     "3.0 Music",  3 },
        { SpeakerArr::k31Cine,      "3.1 Cine",   4 },
        { SpeakerArr::k31Music,     "3.1 Music",  4 },
        { SpeakerArr::k40Cine,      "4.0 Cine",   4 },
        { SpeakerArr::k40Music,     "4.0 Music",  4 },
        { SpeakerArr::k41Cine,      "4.1 Cine",   5 },
        { SpeakerArr::k41Music,     "4.1 Music",  5 },
        { SpeakerArr::k50,          "5.0",        5 },
        { SpeakerArr::k51,          "5.1",        6 },
        { SpeakerArr::k60Cine,      "6.0 Cine",   6 },
        { SpeakerArr::k60Music,     "6.0 Music",  6 },
        { SpeakerArr::k61Cine,      "6.1 Cine",   7 },
        { SpeakerArr::k61Music,     "6.1 Music",  7 },
        { SpeakerArr::k70Cine,      "7.0 Cine",   7 },
        { SpeakerArr::k70Music,     "7.0 Music",  7 },
        { SpeakerArr::k71Cine,      "7.1 Cine",   8 },
        { SpeakerArr::k71Music,     "7.1 Music",  8 },
        { SpeakerArr::k80Cine,      "8.0 Cine",   8 },
        { SpeakerArr::k80Music,     "8.0 Music",  8 },
        { SpeakerArr::k81Cine,      "8.1 Cine",   9 },
        { SpeakerArr::k81Music,     "8.1 Music",  9 },

        // Extended formats
        { SpeakerArr::k71CineTopCenter,     "7.1 Cine Top Center",      8 },
        { SpeakerArr::k71CineCenterHigh,    "7.1 Cine Center High",     8 },
        { SpeakerArr::k71CineFrontHigh,     "7.1 Cine Front High",      8 },
        { SpeakerArr::k71CineSideHigh,      "7.1 Cine Side High",       8 },
        { SpeakerArr::k71CineFullRear,      "7.1 Cine Full Rear",       8 },
        { SpeakerArr::k90Cine,      "9.0 Cine",   9 },
        { SpeakerArr::k91Cine,      "9.1 Cine",   10 },
        { SpeakerArr::k100Cine,     "10.0 Cine",  10 },
        { SpeakerArr::k101Cine,     "10.1 Cine",  11 },

        // Atmos formats
        { SpeakerArr::k50_2,        "5.0.2",      7 },
        { SpeakerArr::k51_2,        "5.1.2",      8 },
        { SpeakerArr::k50_4,        "5.0.4",      9 },
        { SpeakerArr::k51_4,        "5.1.4",      10 },
        { SpeakerArr::k70_2,        "7.0.2",      9 },
        { SpeakerArr::k71_2,        "7.1.2",      10 },
        { SpeakerArr::k70_4,        "7.0.4",      11 },
        { SpeakerArr::k71_4,        "7.1.4",      12 },
        { SpeakerArr::k90_4,        "9.0.4",      13 },
        { SpeakerArr::k91_4,        "9.1.4",      14 },
        { SpeakerArr::k70_6,        "7.0.6",      13 },
        { SpeakerArr::k71_6,        "7.1.6",      14 },
        { SpeakerArr::k90_6,        "9.0.6",      15 },
        { SpeakerArr::k91_6,        "9.1.6",      16 },

        // Quadraphonic
        { SpeakerArr::k40_4,        "4.0.4",      8 },

        // Ambisonics (1st - 7th order)
        { SpeakerArr::kAmbi1stOrderACN, "Ambi 1st Order", 4 },
        { SpeakerArr::kAmbi2cdOrderACN, "Ambi 2nd Order", 9 },
        { SpeakerArr::kAmbi3rdOrderACN, "Ambi 3rd Order", 16 },
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
}

remidy::AudioChannelLayout fromVst3SpeakerArrangement(SpeakerArrangement src) {
    // Try to find exact match in mapping table
    for (const auto& mapping : arrangementMappings) {
        if (mapping.arrangement == src) {
            return remidy::AudioChannelLayout{mapping.name, mapping.channels};
        }
    }

    // Fallback: count channels and return generic layout
    uint32_t channels = countChannels (src);
    return layoutForChannels (channels);
}

SpeakerArrangement toVstSpeakerArrangement(remidy::AudioChannelLayout src) {
    uint32_t channels = src.channels();

    // Handle empty layout
    if (channels == 0)
        return SpeakerArr::kEmpty;

    // Try to find matching arrangement by name first (if name is specified)
    const auto& srcName = const_cast<remidy::AudioChannelLayout&>(src).name();
    if (!srcName.empty()) {
        for (const auto& mapping : arrangementMappings) {
            if (mapping.channels == channels && srcName == mapping.name) {
                return mapping.arrangement;
            }
        }
    }

    // Fallback to standard layouts based on channel count
    switch (channels) {
        case 1: return SpeakerArr::kMono;
        case 2: return SpeakerArr::kStereo;
        case 3: return SpeakerArr::k30Music;     // L R C
        case 4: return SpeakerArr::k40Music;     // L R Ls Rs
        case 5: return SpeakerArr::k50;          // L R C Ls Rs
        case 6: return SpeakerArr::k51;          // L R C LFE Ls Rs
        case 7: return SpeakerArr::k70Music;     // L R C Ls Rs Lc Rc
        case 8: return SpeakerArr::k71Music;     // L R C LFE Ls Rs Lc Rc
        case 9: return SpeakerArr::k81Music;     // L R C LFE Ls Rs Lc Rc Cs
        case 10: return SpeakerArr::k91Cine;     // L R C LFE Ls Rs Lcs Rcs Lw Rw
        default:
            // For other channel counts, return empty (plugin will reject or handle)
            return SpeakerArr::kEmpty;
    }
}

void remidy::PluginInstanceVST3::AudioBuses::inspectBuses() {
    auto component = owner->component;
    auto processor = owner->processor;

    BusSearchResult ret{};
    auto numAudioIn = component->getBusCount(kAudio, kInput);
    auto numAudioOut = component->getBusCount(kAudio, kOutput);
    ret.numAudioIn = static_cast<uint32_t>(numAudioIn);
    ret.numAudioOut = static_cast<uint32_t>(numAudioOut);
    ret.numEventIn = component->getBusCount(kEvent, kInput);
    ret.numEventOut = component->getBusCount(kEvent, kOutput);

    input_bus_defs.clear();
    output_bus_defs.clear();
    for (auto bus: audio_in_buses)
        delete bus;
    for (auto bus: audio_out_buses)
        delete bus;
    audio_in_buses.clear();
    audio_out_buses.clear();
    BusInfo info;
    for (uint32_t bus = 0; bus < numAudioIn; bus++) {
        component->getBusInfo(kAudio, kInput, bus, info);
        auto name = vst3StringToStdString(info.name);
        auto def = AudioBusDefinition{name, info.flags & BusInfo::kDefaultActive ? AudioBusRole::Main : AudioBusRole::Aux};
        input_bus_defs.emplace_back(def);
        auto conf = new AudioBusConfiguration(def);
        SpeakerArrangement arr;
        processor->getBusArrangement(kInput, bus, arr);
        conf->channelLayout(fromVst3SpeakerArrangement(arr));
        audio_in_buses.emplace_back(conf);
    }
    for (uint32_t bus = 0; bus < numAudioOut; bus++) {
        component->getBusInfo(kAudio, kOutput, bus, info);
        auto name = vst3StringToStdString(info.name);
        auto def = AudioBusDefinition{name, info.flags & BusInfo::kDefaultActive ? AudioBusRole::Main : AudioBusRole::Aux};
        output_bus_defs.emplace_back(def);
        auto conf = new AudioBusConfiguration(def);
        SpeakerArrangement arr;
        processor->getBusArrangement(kOutput, bus, arr);
        conf->channelLayout(fromVst3SpeakerArrangement(arr));
        audio_out_buses.emplace_back(conf);
    }

    busesInfo = ret;
}

void remidy::PluginInstanceVST3::AudioBuses::configure(remidy::PluginInstance::ConfigurationRequest &config) {
    auto component = owner->component;
    auto processor = owner->processor;
    auto logger = owner->owner->getLogger();

    auto applyRequestedChannels = [&](std::vector<AudioBusConfiguration*>& buses, int32_t busIndex, const std::optional<uint32_t>& requested, const char* label) {
        if (!requested.has_value())
            return;
        if (busIndex < 0 || static_cast<size_t>(busIndex) >= buses.size())
            return;
        auto bus = buses[static_cast<size_t>(busIndex)];
        auto channels = requested.value();
        bus->enabled(channels > 0);
        if (channels == 0)
            return;
        if (channels > 2) {
            logger->logWarning("%s: Requested %u channels on %s bus; keeping plugin-provided configuration", owner->pluginName.c_str(), channels, label);
            return;
        }
        auto layout = layoutForChannels(channels);
        if (bus->channelLayout(layout) != StatusCode::OK)
            bus->channelLayout() = layout;
    };

    applyRequestedChannels(audio_in_buses, mainInputBusIndex(), config.mainInputChannels, "input");
    applyRequestedChannels(audio_out_buses, mainOutputBusIndex(), config.mainOutputChannels, "output");

    std::vector<SpeakerArrangement> inArr{};
    inArr.reserve(audio_in_buses.size());
    for (const auto &input_bus : audio_in_buses)
        inArr.emplace_back(toVstSpeakerArrangement(input_bus->channelLayout()));
    std::vector<SpeakerArrangement> outArr{};
    outArr.reserve(audio_out_buses.size());
    for (const auto &output_bus : audio_out_buses)
        outArr.emplace_back(toVstSpeakerArrangement(output_bus->channelLayout()));

    // Some plug-ins will crash if you pass a nullptr to setBusArrangements
    SpeakerArrangement nullArrangement = {};
    auto* inData = inArr.empty() ? &nullArrangement : inArr.data();
    auto* outData = outArr.empty() ? &nullArrangement : outArr.data();

    // set audio bus configuration, if explicitly specified.
    auto result = processor->setBusArrangements(inData, static_cast<int32_t>(inArr.size()),
                                                outData, static_cast<int32_t>(outArr.size()));
    if (result != kResultOk) {
        logger->logWarning("%s: setBusArrangements returned %d; falling back to plugin defaults", owner->pluginName.c_str(), result);
        inspectBuses();
    } else {
        // Verify that the plugin actually applied what we requested
        bool mismatch = false;
        for (size_t i = 0; i < inArr.size(); ++i) {
            SpeakerArrangement actual;
            if (processor->getBusArrangement(kInput, static_cast<int32_t>(i), actual) == kResultOk) {
                if (actual != inArr[i]) {
                    logger->logWarning("%s: Input bus %zu arrangement mismatch (requested vs actual)", owner->pluginName.c_str(), i);
                    mismatch = true;
                }
            }
        }
        for (size_t i = 0; i < outArr.size(); ++i) {
            SpeakerArrangement actual;
            if (processor->getBusArrangement(kOutput, static_cast<int32_t>(i), actual) == kResultOk) {
                if (actual != outArr[i]) {
                    logger->logWarning("%s: Output bus %zu arrangement mismatch (requested vs actual)", owner->pluginName.c_str(), i);
                    mismatch = true;
                }
            }
        }
        if (mismatch) {
            // Reload actual configuration from plugin
            inspectBuses();
        }
    }
    for (size_t i = 0, n = audio_in_buses.size(); i < n; ++i) {
        const bool active = audio_in_buses[i]->enabled() && audio_in_buses[i]->channelLayout().channels() > 0;
        component->activateBus(kAudio, kInput, static_cast<int32_t>(i), active);
    }
    for (size_t i = 0, n = audio_out_buses.size(); i < n; ++i) {
        const bool active = audio_out_buses[i]->enabled() && audio_out_buses[i]->channelLayout().channels() > 0;
        component->activateBus(kAudio, kOutput, static_cast<int32_t>(i), active);
    }
}

void remidy::PluginInstanceVST3::AudioBuses::deactivateAllBuses() {
    auto component = owner->component;
    for (size_t i = 0, n = audio_in_buses.size(); i < n; ++i)
        component->activateBus(kAudio, kInput, i, false);
    for (size_t i = 0, n = audio_out_buses.size(); i < n; ++i)
        component->activateBus(kAudio, kOutput, i, false);
    for (size_t i = 0, n = busesInfo.numEventIn; i < n; ++i)
        component->activateBus(kEvent, kInput, i, false);
    for (size_t i = 0, n = busesInfo.numEventOut; i < n; ++i)
        component->activateBus(kEvent, kOutput, i, false);
}

void remidy::PluginInstanceVST3::AudioBuses::allocateBuffers() {
    auto& processData = owner->processData;

    // FIXME: adjust audio buses and channels appropriately.

    inputAudioBusBuffersList.resize(audio_in_buses.size());
    outputAudioBusBuffersList.resize(audio_out_buses.size());

    int32_t numInputBuses = audio_in_buses.size();
    int32_t numOutputBuses = audio_out_buses.size();
    processData.numInputs = numInputBuses;
    processData.numOutputs = numOutputBuses;
    processData.inputs = inputAudioBusBuffersList.data();
    processData.outputs = outputAudioBusBuffersList.data();
    int32_t symbolicSampleSize = processData.symbolicSampleSize;
    for (int32_t bus = 0; bus < numInputBuses; bus++) {
        int32_t numChannels = static_cast<int32_t>(audio_in_buses[bus]->channelLayout().channels());
        inputAudioBusBuffersList[bus].numChannels = numChannels;
        if (numChannels <= 0)
            continue;
        if (symbolicSampleSize == kSample32)
            inputAudioBusBuffersList[bus].channelBuffers32 = (float **) calloc(static_cast<size_t>(numChannels), sizeof(float *));
        else
            inputAudioBusBuffersList[bus].channelBuffers64 = (double **) calloc(static_cast<size_t>(numChannels), sizeof(double *));
    }
    for (int32_t bus = 0; bus < numOutputBuses; bus++) {
        int32_t numChannels = static_cast<int32_t>(audio_out_buses[bus]->channelLayout().channels());
        outputAudioBusBuffersList[bus].numChannels = numChannels;
        if (numChannels <= 0)
            continue;
        if (symbolicSampleSize == kSample32)
            outputAudioBusBuffersList[bus].channelBuffers32 = (float **) calloc(static_cast<size_t>(numChannels), sizeof(float *));
        else
            outputAudioBusBuffersList[bus].channelBuffers64 = (double **) calloc(static_cast<size_t>(numChannels), sizeof(double *));
    }
}

// We cannot "free" pointers on processData because they might get updated by the
// plugin instance (e.g. by "processReplacing").
// We allocate memory in inputAudioBusBuffersList and outputAudioBusBuffersList.
void remidy::PluginInstanceVST3::AudioBuses::deallocateBuffers() {
    auto& processData = owner->processData;

    // FIXME: adjust audio buses and channels
    int32_t numInputBuses = audio_in_buses.size();
    int32_t numOutputBuses = audio_out_buses.size();
    int32_t symbolicSampleSize = processData.symbolicSampleSize;
    if (symbolicSampleSize == kSample32) {
        for (int32_t bus = 0; bus < numInputBuses; bus++)
            if (inputAudioBusBuffersList[bus].channelBuffers32)
                free(inputAudioBusBuffersList[bus].channelBuffers32);
    } else {
        for (int32_t bus = 0; bus < numInputBuses; bus++)
            if (inputAudioBusBuffersList[bus].channelBuffers64)
                free(inputAudioBusBuffersList[bus].channelBuffers64);
    }
    if (symbolicSampleSize == kSample32) {
        for (int32_t bus = 0; bus < numOutputBuses; bus++)
            if (outputAudioBusBuffersList[bus].channelBuffers32)
                free(outputAudioBusBuffersList[bus].channelBuffers32);
    } else {
        for (int32_t bus = 0; bus < numOutputBuses; bus++)
            if (outputAudioBusBuffersList[bus].channelBuffers64)
                free(outputAudioBusBuffersList[bus].channelBuffers64);
    }
}
