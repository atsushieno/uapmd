
#include <iostream>

#include "remidy.hpp"
#include "../utils.hpp"

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

// FIXME: we should make edit controller lazily loaded.
//  Some plugins take long time to instantiate IEditController, and it does not make sense for
//  non-UI-based audio processing like our virtual MIDI devices.
remidy::PluginInstanceVST3::PluginInstanceVST3(
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
#if 1
    // If we disable this, those JUCE plugins cannot get parameters.
    // If we enable this, Serum2 and Sforzando crashes.
    if (isControllerDistinctFromComponent && connPointComp && connPointComp->vtable && connPointEdit && connPointEdit->vtable) {
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
    }
#endif

    audio_buses = new AudioBuses(this);

    // find NoteExpressionController
    if (controller->vtable->unknown.query_interface(controller, v3_note_expression_controller_iid, (void**) &note_expression_controller) != V3_OK)
        note_expression_controller = nullptr; // just to make sure
    if (controller->vtable->unknown.query_interface(controller, v3_unit_information_iid, (void**) &unit_info) != V3_OK)
        unit_info = nullptr; // just to make sure
    if (controller->vtable->unknown.query_interface(controller, v3_midi_mapping_iid, (void**) &midi_mapping) != V3_OK)
        midi_mapping = nullptr; // just to make sure

    // not sure if we want to error out here, so no result check.
    processor->vtable->processor.set_processing(processor, false);
    component->vtable->component.set_active(component, false);
}

remidy::PluginInstanceVST3::~PluginInstanceVST3() {
    auto logger = owner->getLogger();

    auto result = processor->vtable->processor.set_processing(processor, false);
    if (result != V3_OK)
        logger->logError("Failed to setProcessing(false) at VST3 destructor: %d", result);
    result = component->vtable->component.set_active(component, false);
    if (result != V3_OK)
        logger->logError("Failed to setActive(false) at VST3 destructor: %d", result);
    audio_buses->deactivateAllBuses();

    if (isControllerDistinctFromComponent && connPointComp && connPointEdit) {
        result = connPointEdit->vtable->connection_point.disconnect(connPointEdit, (v3_connection_point**) connPointComp);
        if (result != V3_OK)
            logger->logError("Failed to disconnect from Component ConnectionPoint at VST3 destructor: %d", result);
        result = connPointComp->vtable->connection_point.disconnect(connPointComp, (v3_connection_point**) connPointEdit);
        if (result != V3_OK)
            logger->logError("Failed to disconnect from EditController ConnectionPoint at VST3 destructor: %d", result);
    }
    // FIXME: almost all plugins crash here.
    controller->vtable->controller.set_component_handler(controller, nullptr);

    if (isControllerDistinctFromComponent) {
        controller->vtable->base.terminate(controller);
    }
    component->vtable->base.terminate(component);

    audio_buses->deallocateBuffers();

    if (connPointEdit)
        connPointEdit->vtable->unknown.unref(connPointEdit);
    if (connPointComp)
        connPointComp->vtable->unknown.unref(connPointComp);

    processor->vtable->unknown.unref(processor);
    if (isControllerDistinctFromComponent)
        controller->vtable->unknown.unref(controller);
    component->vtable->unknown.unref(component);
    instance->vtable->unknown.unref(instance);

    owner->unrefLibrary(info());

    delete _parameters;
}

remidy::StatusCode remidy::PluginInstanceVST3::configure(ConfigurationRequest &configuration) {
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

void remidy::PluginInstanceVST3::allocateProcessData(v3_process_setup& setup) {
    processData.ctx = &process_context;
    process_context.sample_rate = setup.sample_rate;

    processData.input_events = (v3_event_list **) processDataInputEvents.asInterface();
    processData.output_events = (v3_event_list **) processDataOutputEvents.asInterface();
    // FIXME: we should reconsider how we pass it.
    processData.input_params = (v3_param_changes **) processDataInputParameterChanges.asInterface();
    processData.output_params = (v3_param_changes **) processDataOutputParameterChanges.asInterface();

    audio_buses->allocateBuffers();

    processData.process_mode = setup.process_mode;
    processData.symbolic_sample_size = setup.symbolic_sample_size;
}

remidy::StatusCode remidy::PluginInstanceVST3::startProcessing() {
    // we need to allocate memory where necessary.
    owner->getHost()->startProcessing();

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

remidy::StatusCode remidy::PluginInstanceVST3::stopProcessing() {
    auto result = processor->vtable->processor.set_processing(processor, false);
    // regarding V3_NOT_IMPLEMENTED, see startProcessing().
    if (result != V3_OK && result != V3_NOT_IMPLEMENTED) {
        owner->getLogger()->logError("Failed to stop vst3 processing. Result: %d", result);
        return StatusCode::FAILED_TO_STOP_PROCESSING;
    }

    component->vtable->component.set_active(component, false);

    // we deallocate memory where necessary.
    owner->getHost()->stopProcessing();

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::process(AudioProcessContext &process) {
    // update audio buffer pointers
    const int32_t numFrames = process.frameCount();
    const int32_t numInputBus = processData.num_input_buses;
    const int32_t numOutputBus = processData.num_output_buses;
    for (int32_t bus = 0, nBus = process.audioInBusCount(); bus < nBus; bus++) {
        if (bus >= numInputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more input buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numInputBus);
            continue;
        }
        for (size_t ch = 0, n = process.inputChannelCount(bus); ch < n; ch++) {
            if (processData.symbolic_sample_size == V3_SAMPLE_32)
                processData.inputs[bus].channel_buffers_32[ch] = process.getFloatInBuffer(bus, ch);
            else
                processData.inputs[bus].channel_buffers_64[ch] = process.getDoubleInBuffer(bus, ch);
        }
    }
    for (int32_t bus = 0, nBus = process.audioOutBusCount(); bus < nBus; bus++) {
        if (bus >= numOutputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more output buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numOutputBus);
            continue;
        }
        for (size_t ch = 0, n = process.outputChannelCount(bus); ch < n; ch++) {
            if (processData.symbolic_sample_size == V3_SAMPLE_32)
                processData.outputs[bus].channel_buffers_32[ch] = process.getFloatOutBuffer(bus, ch);
            else
                processData.outputs[bus].channel_buffers_64[ch] = process.getDoubleOutBuffer(bus, ch);
        }
    }

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

remidy::PluginParameterSupport* remidy::PluginInstanceVST3::parameters() {
    if (!_parameters)
        _parameters = new ParameterSupport(this);
    return _parameters;
}
