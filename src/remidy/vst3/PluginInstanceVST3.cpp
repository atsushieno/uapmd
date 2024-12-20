
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

namespace remidy {
    // FIXME: we should make edit controller lazily loaded.
    //  Some plugins take long time to instantiate IEditController, and it does not make sense for
    //  non-UI-based audio processing like our virtual MIDI devices.
    AudioPluginInstanceVST3::AudioPluginInstanceVST3(
            PluginFormatVST3::Impl *owner,
            PluginCatalogEntry *info,
            void *module,
            IPluginFactory *factory,
            IComponent *component,
            IAudioProcessor *processor,
            IEditController *controller,
            bool isControllerDistinctFromComponent,
            FUnknown *instance
    ) : PluginInstance(info), owner(owner), module(module), factory(factory),
        component(component), processor(processor), controller(controller),
        isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {

        pluginName = info->displayName();

        // set up IConnectionPoints
        auto result = component->vtable->unknown.query_interface(component, v3_connection_point_iid,
                                                                 (void **) &connPointComp);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError(
                    "%s: IComponent failed to return query for IConnectionPoint as expected. Result: %d",
                    pluginName.c_str(), result);
        result = controller->vtable->unknown.query_interface(controller, v3_connection_point_iid,
                                                             (void **) &connPointEdit);
        if (result != V3_OK && result != V3_NO_INTERFACE)
            owner->getLogger()->logError(
                    "%s: IEditController failed to return query for IConnectionPoint as expected. Result: %d",
                    pluginName.c_str(), result);

        // From JUCE interconnectComponentAndController():
        // > Some plugins need to be "connected" to intercommunicate between their implemented classes
        /*
        if (isControllerDistinctFromComponent && connPointComp && connPointComp->vtable && connPointEdit && connPointEdit->vtable) {
            std::atomic<bool> waitHandle{false};
            EventLoop::runTaskOnMainThread([&] {
                result = connPointComp->vtable->connection_point.connect(connPointComp, (v3_connection_point**) connPointEdit);
                if (result != V3_OK) {
                    owner->getLogger()->logWarning(
                            "%s: IConnectionPoint from IComponent failed to interconnect with its IConnectionPoint from IEditController. Result: %d",
                            pluginName.c_str(), result);
                }
                result = connPointEdit->vtable->connection_point.connect(connPointEdit, (v3_connection_point**) connPointComp);
                if (result != V3_OK) {
                    owner->getLogger()->logWarning(
                            "%s: IConnectionPoint from IEditController failed to interconnect with its IConnectionPoint from IComponent. Result: %d",
                            pluginName.c_str(), result);
                }
                waitHandle = true;
                waitHandle.notify_one();
            });
            while (!waitHandle)
                std::this_thread::yield();
        }*/

        // not sure if we want to error out here, so no result check.
        processor->vtable->processor.set_processing(processor, false);
        component->vtable->component.set_active(component, false);

        inspectBuses();
    }

    AudioPluginInstanceVST3::~AudioPluginInstanceVST3() {

        std::function releaseRemaining = [this] {
            processor->vtable->processor.set_processing(processor, false);
            component->vtable->component.set_active(component, false);

            deallocateProcessData();

            if (connPointEdit)
                connPointEdit->vtable->unknown.unref(connPointEdit);
            if (connPointComp)
                connPointComp->vtable->unknown.unref(connPointComp);

            processor->vtable->unknown.unref(processor);

            component->vtable->base.terminate(component);
            component->vtable->unknown.unref(component);

            instance->vtable->unknown.unref(instance);

            owner->unrefLibrary(info());
        };

        std::cerr << "VST3 instance destructor: " << info()->displayName() << std::endl;
        std::atomic<bool> waitHandle{false};
        EventLoop::runTaskOnMainThread([&] {
            if (isControllerDistinctFromComponent) {
                controller->vtable->base.terminate(controller);
                controller->vtable->unknown.unref(controller);
            }
            releaseRemaining();
            waitHandle = true;
            waitHandle.notify_one();
        });
        std::cerr << "  waiting for cleanup: " << info()->displayName() << std::endl;
        while (!waitHandle)
            std::this_thread::yield();
        std::cerr << "  cleanup done: " << info()->displayName() << std::endl;

        for (const auto bus: input_buses)
            delete bus;
        for (const auto bus: output_buses)
            delete bus;

        if (_parameters)
            delete _parameters;
    }

    v3_speaker_arrangement toVstSpeakerArrangement(AudioChannelLayout src) {
        v3_speaker_arrangement ret{0};
        if (src == AudioChannelLayout::mono())
            ret = V3_SPEAKER_C;
        else if (src == AudioChannelLayout::stereo())
            ret = V3_SPEAKER_L | V3_SPEAKER_R;
        // FIXME: implement more
        return ret;
    }

    StatusCode AudioPluginInstanceVST3::configure(ConfigurationRequest &configuration) {
        // setupProcessing.
        v3_process_setup setup{};
        setup.sample_rate = configuration.sampleRate;
        setup.max_block_size = static_cast<int32_t>(configuration.bufferSizeInSamples);
        setup.symbolic_sample_size = configuration.dataType == AudioContentType::Float64 ? V3_SAMPLE_64 : V3_SAMPLE_32;
        setup.process_mode = configuration.offlineMode ? V3_OFFLINE : V3_REALTIME;

        auto result = processor->vtable->processor.setup_processing(processor, &setup);
        if (result != V3_OK) {
            owner->getLogger()->logError("Failed to call vst3 setup_processing() result: %d", result);
            return StatusCode::FAILED_TO_CONFIGURE;
        }

        std::vector<v3_speaker_arrangement> inArr{input_buses.size()};
        for (const auto &input_buse: input_buses)
            inArr.emplace_back(toVstSpeakerArrangement(input_buse->channelLayout()));
        std::vector<v3_speaker_arrangement> outArr{output_buses.size()};
        for (const auto &output_buse: output_buses)
            outArr.emplace_back(toVstSpeakerArrangement(output_buse->channelLayout()));

        // set audio bus configuration, if explicitly specified.
        processor->vtable->processor.set_bus_arrangements(processor,
                                                          inArr.data(), static_cast<int32_t>(inArr.size()),
                                                          outArr.data(), static_cast<int32_t>(outArr.size()));
        for (size_t i = 0, n = input_buses.size(); i < n; ++i)
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_INPUT, i, input_buses[i]->enabled());
        for (size_t i = 0, n = output_buses.size(); i < n; ++i)
            component->vtable->component.activate_bus(component, V3_AUDIO, V3_OUTPUT, i, output_buses[i]->enabled());

        // setup process_data here.
        allocateProcessData(setup);

        return StatusCode::OK;
    }

    void AudioPluginInstanceVST3::allocateProcessData(v3_process_setup& setup) {
        // FIXME: retrieve these properties by some means.
        processData.ctx = &process_context;
        process_context.sample_rate = setup.sample_rate;

        processData.input_events = (v3_event_list **) processDataInputEvents.asInterface();
        processData.output_events = (v3_event_list **) processDataOutputEvents.asInterface();
        processData.input_params = (v3_param_changes **) processDataInputParameterChanges.asInterface();
        processData.output_params = (v3_param_changes **) processDataOutputParameterChanges.asInterface();

        // FIXME: adjust audio buses and channels appropriately.
        inputAudioBusBuffersList.resize(input_buses.size());
        outputAudioBusBuffersList.resize(output_buses.size());

        int32_t numInputBuses = input_buses.size();
        int32_t numOutputBuses = output_buses.size();
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

        processData.process_mode = V3_REALTIME; // FIXME: assign specified value
        processData.symbolic_sample_size = V3_SAMPLE_32; // FIXME: assign specified value
    }

    // We cannot "free" pointers on processData because they might get updated by the
    // plugin instance (e.g. by "processReplacing").
    // We allocate memory in inputAudioBusBuffersList and outputAudioBusBuffersList.
    void AudioPluginInstanceVST3::deallocateProcessData() {
        // FIXME: adjust audio buses and channels
        int32_t numInputBuses = input_buses.size();
        int32_t numOutputBuses = output_buses.size();
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

    StatusCode AudioPluginInstanceVST3::startProcessing() {
        // activate the instance.
        component->vtable->component.set_active(component, true);

        auto result = processor->vtable->processor.set_processing(processor, true);
        // Surprisingly?, some VST3 plugins do not implement this function.
        // We do not prevent them just because of this.
        if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
            owner->getLogger()->logError("Failed to start vst3 processing. Result: %d", result);
            return StatusCode::FAILED_TO_START_PROCESSING;
        }
        return StatusCode::OK;
    }

    StatusCode AudioPluginInstanceVST3::stopProcessing() {
        auto result = processor->vtable->processor.set_processing(processor, false);
        // regarding V3_NOT_IMPLEMENTED, see startProcessing().
        if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
            owner->getLogger()->logError("Failed to stop vst3 processing. Result: %d", result);
            return StatusCode::FAILED_TO_STOP_PROCESSING;
        }

        component->vtable->component.set_active(component, false);
        return StatusCode::OK;
    }

    void
    updateProcessDataBuffers(v3_process_data &processData, v3_audio_bus_buffers &dstBus, AudioBusBufferList *srcBuf) {
        int32_t numChannels = dstBus.num_channels;
        if (processData.symbolic_sample_size == V3_SAMPLE_32) {
            for (int32_t ch = 0; ch < numChannels; ch++)
                dstBus.channel_buffers_32[ch] = srcBuf->getFloatBufferForChannel(ch);
        } else {
            for (int32_t ch = 0; ch < numChannels; ch++)
                dstBus.channel_buffers_64[ch] = srcBuf->getDoubleBufferForChannel(ch);
        }
    }

    StatusCode AudioPluginInstanceVST3::process(AudioProcessContext &process) {
        // update audio buffer pointers
        const int32_t numFrames = process.frameCount();
        const int32_t numInputBus = processData.num_input_buses;
        const int32_t numOutputBus = processData.num_output_buses;
        for (int32_t bus = 0; bus < numInputBus; bus++)
            updateProcessDataBuffers(processData, processData.inputs[bus], process.audioIn(bus));
        for (int32_t bus = 0; bus < numOutputBus; bus++)
            updateProcessDataBuffers(processData, processData.outputs[bus], process.audioOut(bus));

        const auto &ctx = processData.ctx;

        processData.nframes = numFrames;

        // handle UMP inputs via UmpInputDispatcher.
        processDataInputEvents.clear();
        // FIXME: pass correct timestamp
        ump_input_dispatcher.process(0, process);

        // invoke plugin process
        auto result = processor->vtable->processor.process(processor, &processData);

        if (result != V3_OK) {
            owner->getLogger()->logError("Failed to process vst3 audio. Result: %d", result);
            return StatusCode::FAILED_TO_PROCESS;
        }

        // post-processing
        ctx->continuous_time_in_samples += numFrames;

        // FiXME: generate UMP outputs here
        processDataOutputEvents.clear();

        return StatusCode::OK;
    }

    AudioChannelLayout fromVst3SpeakerArrangement(v3_speaker_arrangement src) {
        uint32_t channels = 0;
        for (int32_t i = 0; i < 19; i++)
            if (src & (1 << i))
                channels++;
        return AudioChannelLayout{"", channels};
    }

    void AudioPluginInstanceVST3::inspectBuses() {
        BusSearchResult ret{};
        auto numAudioIn = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_INPUT);
        auto numAudioOut = component->vtable->component.get_bus_count(component, V3_AUDIO, V3_OUTPUT);
        ret.numEventIn = component->vtable->component.get_bus_count(component, V3_EVENT, V3_INPUT);
        ret.numEventOut = component->vtable->component.get_bus_count(component, V3_EVENT, V3_OUTPUT);

        input_bus_defs.clear();
        output_bus_defs.clear();
        for (auto bus: input_buses)
            delete bus;
        for (auto bus: output_buses)
            delete bus;
        input_buses.clear();
        output_buses.clear();
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
            input_buses.emplace_back(conf);
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
            output_buses.emplace_back(conf);
        }

        busesInfo = ret;
    }

    const std::vector<AudioBusConfiguration*>& AudioPluginInstanceVST3::audioInputBuses() const { return input_buses; }

    const std::vector<AudioBusConfiguration*>& AudioPluginInstanceVST3::audioOutputBuses() const { return output_buses; }

    PluginParameterSupport* AudioPluginInstanceVST3::parameters() {
        if (!_parameters)
            _parameters = new ParameterSupport(this);
        return _parameters;
    }
}
