
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

// FIXME: we should make edit controller lazily loaded.
//  Some plugins take long time to instantiate IEditController, and it does not make sense for
//  non-UI-based audio processing like our virtual MIDI devices.
remidy::AudioPluginInstanceVST3::AudioPluginInstanceVST3(
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
    isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance),
    audio_buses(new VST3AudioBuses(this)) {

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
}

remidy::AudioPluginInstanceVST3::~AudioPluginInstanceVST3() {

    std::function releaseRemaining = [this] {
        processor->vtable->processor.set_processing(processor, false);
        component->vtable->component.set_active(component, false);

        audio_buses->deallocateBuffers();

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

    delete audio_buses;

    delete _parameters;
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::configure(ConfigurationRequest &configuration) {
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

    // setup audio buses
    audio_buses->configure(configuration);

    // setup process_data here.
    allocateProcessData(setup);

    return StatusCode::OK;
}

void remidy::AudioPluginInstanceVST3::allocateProcessData(v3_process_setup& setup) {
    // FIXME: retrieve these properties by some means.
    processData.ctx = &process_context;
    process_context.sample_rate = setup.sample_rate;

    processData.input_events = (v3_event_list **) processDataInputEvents.asInterface();
    processData.output_events = (v3_event_list **) processDataOutputEvents.asInterface();
    processData.input_params = (v3_param_changes **) processDataInputParameterChanges.asInterface();
    processData.output_params = (v3_param_changes **) processDataOutputParameterChanges.asInterface();

    audio_buses->allocateBuffers();

    processData.process_mode = V3_REALTIME; // FIXME: assign specified value
    processData.symbolic_sample_size = V3_SAMPLE_32; // FIXME: assign specified value
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::startProcessing() {
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

remidy::StatusCode remidy::AudioPluginInstanceVST3::stopProcessing() {
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
updateProcessDataBuffers(v3_process_data &processData, v3_audio_bus_buffers &dstBus, remidy::AudioBusBufferList *srcBuf) {
    int32_t numChannels = dstBus.num_channels;
    if (processData.symbolic_sample_size == V3_SAMPLE_32) {
        for (int32_t ch = 0; ch < numChannels; ch++)
            dstBus.channel_buffers_32[ch] = srcBuf->getFloatBufferForChannel(ch);
    } else {
        for (int32_t ch = 0; ch < numChannels; ch++)
            dstBus.channel_buffers_64[ch] = srcBuf->getDoubleBufferForChannel(ch);
    }
}

remidy::StatusCode remidy::AudioPluginInstanceVST3::process(AudioProcessContext &process) {
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

remidy::PluginParameterSupport* remidy::AudioPluginInstanceVST3::parameters() {
    if (!_parameters)
        _parameters = new ParameterSupport(this);
    return _parameters;
}
