#include "PluginFormatVST3.hpp"

remidy::AudioChannelLayout fromVst3SpeakerArrangement(v3_speaker_arrangement src) {
    uint32_t channels = 0;
    for (int32_t i = 0; i < 19; i++)
        if (src & (1 << i))
            channels++;
    return remidy::AudioChannelLayout{"", channels};
}

v3_speaker_arrangement toVstSpeakerArrangement(remidy::AudioChannelLayout src) {
    v3_speaker_arrangement ret{0};
    if (src == remidy::AudioChannelLayout::mono())
        ret = V3_SPEAKER_C;
    else if (src == remidy::AudioChannelLayout::stereo())
        ret = V3_SPEAKER_L | V3_SPEAKER_R;
    // FIXME: implement more
    return ret;
}

void remidy::PluginInstanceVST3::AudioBuses::inspectBuses() {
    auto component = owner->component;
    auto processor = owner->processor;

    BusSearchResult ret{};
    auto numAudioIn = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_INPUT);
    auto numAudioOut = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_OUTPUT);
    ret.numEventIn = component->vtable->component.get_bus_count(component, V3_EVENT, V3_INPUT);
    ret.numEventOut = component->vtable->component.get_bus_count(component, V3_EVENT, V3_OUTPUT);

    input_bus_defs.clear();
    output_bus_defs.clear();
    for (auto bus: audio_in_buses)
        delete bus;
    for (auto bus: audio_out_buses)
        delete bus;
    audio_in_buses.clear();
    audio_out_buses.clear();
    v3_bus_info info;
    for (uint32_t bus = 0; bus < numAudioIn; bus++) {
        component->vtable->component.get_bus_info(component, V3_AUDIO, V3_INPUT, bus, &info);
        auto name = vst3StringToStdString(info.bus_name);
        auto def = AudioBusDefinition{name, info.flags & V3_MAIN ? AudioBusRole::Main : AudioBusRole::Aux};
        input_bus_defs.emplace_back(def);
        auto conf = new AudioBusConfiguration(def);
        v3_speaker_arrangement arr;
        processor->vtable->processor.get_bus_arrangement(processor, V3_INPUT, bus, &arr);
        conf->channelLayout(fromVst3SpeakerArrangement(arr));
        audio_in_buses.emplace_back(conf);
    }
    for (uint32_t bus = 0; bus < numAudioOut; bus++) {
        component->vtable->component.get_bus_info(component, V3_AUDIO, V3_OUTPUT, bus, &info);
        auto name = vst3StringToStdString(info.bus_name);
        auto def = AudioBusDefinition{name, info.flags & V3_MAIN ? AudioBusRole::Main : AudioBusRole::Aux};
        output_bus_defs.emplace_back(def);
        auto conf = new AudioBusConfiguration(def);
        v3_speaker_arrangement arr;
        processor->vtable->processor.get_bus_arrangement(processor, V3_OUTPUT, bus, &arr);
        conf->channelLayout(fromVst3SpeakerArrangement(arr));
        audio_out_buses.emplace_back(conf);
    }

    busesInfo = ret;
}

void remidy::PluginInstanceVST3::AudioBuses::configure(remidy::PluginInstance::ConfigurationRequest &config) {
    auto component = owner->component;
    auto processor = owner->processor;

    std::vector<v3_speaker_arrangement> inArr{audio_in_buses.size()};
    for (const auto &input_buse: audio_in_buses)
        inArr.emplace_back(toVstSpeakerArrangement(input_buse->channelLayout()));
    std::vector<v3_speaker_arrangement> outArr{audio_out_buses.size()};
    for (const auto &output_buse: audio_out_buses)
        outArr.emplace_back(toVstSpeakerArrangement(output_buse->channelLayout()));

    // set audio bus configuration, if explicitly specified.
    processor->vtable->processor.set_bus_arrangements(processor,
                                                      inArr.data(), static_cast<int32_t>(inArr.size()),
                                                      outArr.data(), static_cast<int32_t>(outArr.size()));
    for (size_t i = 0, n = audio_in_buses.size(); i < n; ++i)
        component->vtable->component.activate_bus(component, V3_AUDIO, V3_INPUT, i, audio_in_buses[i]->enabled());
    for (size_t i = 0, n = audio_out_buses.size(); i < n; ++i)
        component->vtable->component.activate_bus(component, V3_AUDIO, V3_OUTPUT, i, audio_out_buses[i]->enabled());
}

void remidy::PluginInstanceVST3::AudioBuses::deactivateAllBuses() {
    auto component = owner->component;
    for (size_t i = 0, n = audio_in_buses.size(); i < n; ++i)
        component->vtable->component.activate_bus(component, V3_AUDIO, V3_INPUT, i, false);
    for (size_t i = 0, n = audio_out_buses.size(); i < n; ++i)
        component->vtable->component.activate_bus(component, V3_AUDIO, V3_OUTPUT, i, false);
    for (size_t i = 0, n = busesInfo.numEventIn; i < n; ++i)
        component->vtable->component.activate_bus(component, V3_EVENT, V3_INPUT, i, false);
    for (size_t i = 0, n = busesInfo.numEventOut; i < n; ++i)
        component->vtable->component.activate_bus(component, V3_EVENT, V3_OUTPUT, i, false);
}

void remidy::PluginInstanceVST3::AudioBuses::allocateBuffers() {
    auto& processData = owner->processData;

    // FIXME: adjust audio buses and channels appropriately.

    inputAudioBusBuffersList.resize(audio_in_buses.size());
    outputAudioBusBuffersList.resize(audio_out_buses.size());

    int32_t numInputBuses = audio_in_buses.size();
    int32_t numOutputBuses = audio_out_buses.size();
    processData.num_input_buses = numInputBuses;
    processData.num_output_buses = numOutputBuses;
    processData.inputs = inputAudioBusBuffersList.data();
    processData.outputs = outputAudioBusBuffersList.data();
    int32_t numChannels = 2;
    int32_t symbolicSampleSize = processData.symbolic_sample_size;
    for (int32_t bus = 0; bus < numInputBuses; bus++) {
        inputAudioBusBuffersList[bus].num_channels = numChannels;
        if (symbolicSampleSize == V3_SAMPLE_32)
            inputAudioBusBuffersList[bus].channel_buffers_32 = (float **) calloc(sizeof(float *), numChannels);
        else
            inputAudioBusBuffersList[bus].channel_buffers_64 = (double **) calloc(sizeof(double *), numChannels);
    }
    for (int32_t bus = 0; bus < numOutputBuses; bus++) {
        outputAudioBusBuffersList[bus].num_channels = numChannels;
        if (symbolicSampleSize == V3_SAMPLE_32)
            outputAudioBusBuffersList[bus].channel_buffers_32 = (float **) calloc(sizeof(float *), numChannels);
        else
            outputAudioBusBuffersList[bus].channel_buffers_64 = (double **) calloc(sizeof(double *), numChannels);
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
    int32_t symbolicSampleSize = processData.symbolic_sample_size;
    if (symbolicSampleSize == V3_SAMPLE_32) {
        for (int32_t bus = 0; bus < numInputBuses; bus++)
            if (inputAudioBusBuffersList[bus].channel_buffers_32)
                free(inputAudioBusBuffersList[bus].channel_buffers_32);
    } else {
        for (int32_t bus = 0; bus < numInputBuses; bus++)
            if (inputAudioBusBuffersList[bus].channel_buffers_64)
                free(inputAudioBusBuffersList[bus].channel_buffers_64);
    }
    if (symbolicSampleSize == V3_SAMPLE_32) {
        for (int32_t bus = 0; bus < numOutputBuses; bus++)
            if (outputAudioBusBuffersList[bus].channel_buffers_32)
                free(outputAudioBusBuffersList[bus].channel_buffers_32);
    } else {
        for (int32_t bus = 0; bus < numOutputBuses; bus++)
            if (outputAudioBusBuffersList[bus].channel_buffers_64)
                free(outputAudioBusBuffersList[bus].channel_buffers_64);
    }
}

