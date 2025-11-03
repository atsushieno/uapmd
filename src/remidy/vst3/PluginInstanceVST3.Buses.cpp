#include "PluginFormatVST3.hpp"
#include <optional>

namespace {
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
    uint32_t channels = 0;
    for (int32_t i = 0; i < 19; i++)
        if (src & (1 << i))
            channels++;
    return remidy::AudioChannelLayout{"", channels};
}

SpeakerArrangement toVstSpeakerArrangement(remidy::AudioChannelLayout src) {
    SpeakerArrangement ret{0};
    if (src.channels() == 0)
        ret = SpeakerArr::kEmpty;
    else if (src == remidy::AudioChannelLayout::mono())
        ret = kSpeakerC;
    else if (src == remidy::AudioChannelLayout::stereo())
        ret = kSpeakerL | kSpeakerR;
    // FIXME: implement more
    return ret;
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

    // set audio bus configuration, if explicitly specified.
    auto result = processor->setBusArrangements(inArr.data(), static_cast<int32_t>(inArr.size()),
                                                outArr.data(), static_cast<int32_t>(outArr.size()));
    if (result != kResultOk) {
        logger->logWarning("%s: setBusArrangements returned %d; falling back to plugin defaults", owner->pluginName.c_str(), result);
        inspectBuses();
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
