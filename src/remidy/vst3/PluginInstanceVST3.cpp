
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
    auto result = component->queryInterface(IConnectionPoint::iid, (void **) &connPointComp);
    if (result != kResultOk && result != kNoInterface)
        owner->getLogger()->logError(
                "%s: IComponent failed to return query for IConnectionPoint as expected. Result: %d",
                pluginName.c_str(), result);
    result = controller->queryInterface(IConnectionPoint::iid, (void **) &connPointEdit);
    if (result != kResultOk && result != kNoInterface)
        owner->getLogger()->logError(
                "%s: IEditController failed to return query for IConnectionPoint as expected. Result: %d",
                pluginName.c_str(), result);

    // From JUCE interconnectComponentAndController():
    // > Some plugins need to be "connected" to intercommunicate between their implemented classes
#if 1
    // If we disable this, those JUCE plugins cannot get parameters.
    // If we enable this, Serum2 and Sforzando crash.
    if (isControllerDistinctFromComponent && connPointComp && connPointEdit) {
        EventLoop::runTaskOnMainThread([&] {
            // You need to understand how those pointer-to-pointer types are used in DPF before attempting to make changes here.
            // Codex is stupid and does not understand why these pointer-to-pointer types are correct.
            result = connPointComp->connect((IConnectionPoint*) connPointEdit);
            if (result != kResultOk) {
                owner->getLogger()->logWarning(
                        "%s: IConnectionPoint from IComponent failed to interconnect with its IConnectionPoint from IEditController. Result: %d",
                        pluginName.c_str(), result);
            }
            result = connPointEdit->connect((IConnectionPoint*) connPointComp);
            if (result != kResultOk) {
                owner->getLogger()->logWarning(
                        "%s: IConnectionPoint from IEditController failed to interconnect with its IConnectionPoint from IComponent. Result: %d",
                        pluginName.c_str(), result);
            }
        });
    }
#endif

    audio_buses = new AudioBuses(this);

    // find NoteExpressionController
    if (controller->queryInterface(INoteExpressionController::iid, (void**) &note_expression_controller) != kResultOk)
        note_expression_controller = nullptr; // just to make sure
    if (controller->queryInterface(IUnitInfo::iid, (void**) &unit_info) != kResultOk)
        unit_info = nullptr; // just to make sure
    if (controller->queryInterface(IMidiMapping::iid, (void**) &midi_mapping) != kResultOk)
        midi_mapping = nullptr; // just to make sure

    // Register parameter edit handler for this plugin instance
    owner->getHost()->setParameterEditHandler(controller, [this](ParamID paramId, double value) {
        // Queue the parameter change for the next audio process call
        auto pvc = processDataInputParameterChanges.asInterface();
        int32_t index = 0;
        auto queue = pvc->addParameterData(paramId, index);
        if (queue) {
            int32_t pointIndex = 0;
            queue->addPoint(0, value, pointIndex);
        }
    });

    // Leave the component inactive until startProcessing() explicitly activates it.
}

remidy::PluginInstanceVST3::~PluginInstanceVST3() {
    auto logger = owner->getLogger();

    // Unregister parameter edit handler
    owner->getHost()->setParameterEditHandler(controller, nullptr);

    auto result = processor->setProcessing(false);
    if (result != kResultOk)
        logger->logError("Failed to setProcessing(false) at VST3 destructor: %d", result);
    EventLoop::runTaskOnMainThread([this, &result, logger] {
        result = component->setActive(false);
        if (result != kResultOk)
            logger->logError("Failed to setActive(false) at VST3 destructor: %d", result);
        audio_buses->deactivateAllBuses();

        if (isControllerDistinctFromComponent && connPointComp && connPointEdit) {
            result = connPointEdit->disconnect((IConnectionPoint*) connPointComp);
            if (result != kResultOk)
                logger->logError("Failed to disconnect from Component ConnectionPoint at VST3 destructor: %d", result);
            result = connPointComp->disconnect((IConnectionPoint*) connPointEdit);
            if (result != kResultOk)
                logger->logError("Failed to disconnect from EditController ConnectionPoint at VST3 destructor: %d",
                                 result);
        }

        // FIXME: almost all plugins crash here. But it seems optional.
        controller->setComponentHandler(nullptr);

        if (isControllerDistinctFromComponent)
            controller->terminate();
        component->terminate();

        audio_buses->deallocateBuffers();

        if (connPointEdit)
            connPointEdit->release();
        if (connPointComp)
            connPointComp->release();

        processor->release();
        if (isControllerDistinctFromComponent)
            controller->release();
        component->release();
        instance->release();
    });

    owner->unrefLibrary(info());

    delete _parameters;
    delete _states;
    delete _presets;
}

remidy::StatusCode remidy::PluginInstanceVST3::configure(ConfigurationRequest &configuration) {
    // setupProcessing.
    ProcessSetup setup{};
    setup.sampleRate = configuration.sampleRate;
    setup.maxSamplesPerBlock = static_cast<int32_t>(configuration.bufferSizeInSamples);
    setup.symbolicSampleSize = configuration.dataType == AudioContentType::Float64 ? kSample64 : kSample32;
    setup.processMode = configuration.offlineMode ? kOffline : kRealtime;

    // setup audio buses
    audio_buses->configure(configuration);

    // setup process_data here.
    allocateProcessData(setup);
    last_process_setup = setup;
    has_process_setup = true;

    return StatusCode::OK;
}

void remidy::PluginInstanceVST3::allocateProcessData(ProcessSetup& setup) {
    processData.processContext = &process_context;
    process_context.sampleRate = setup.sampleRate;

    processData.inputEvents = processDataInputEvents.asInterface();
    processData.outputEvents = processDataOutputEvents.asInterface();
    // FIXME: we should reconsider how we pass it.
    processData.inputParameterChanges = processDataInputParameterChanges.asInterface();
    processData.outputParameterChanges = processDataOutputParameterChanges.asInterface();

    processData.processMode = setup.processMode;
    processData.symbolicSampleSize = setup.symbolicSampleSize;

    audio_buses->allocateBuffers();

    // ensure process data stays in sync with the last setup used during allocation.
}

remidy::StatusCode remidy::PluginInstanceVST3::startProcessing() {
    if (!has_process_setup) {
        owner->getLogger()->logError("%s: startProcessing() called before configure()", pluginName.c_str());
        return StatusCode::FAILED_TO_START_PROCESSING;
    }

    tresult setupResult = kResultOk;
    tresult activationResult = kResultOk;
    bool attemptedActivation = false;
    EventLoop::runTaskOnMainThread([&] {
        owner->getLogger()->logInfo("%s: setting up processing", pluginName.c_str());
        setupResult = processor->setupProcessing(last_process_setup);
        if (setupResult == kResultOk) {
            owner->getLogger()->logInfo("%s: activating component", pluginName.c_str());
            activationResult = component->setActive(true);
            attemptedActivation = true;
        }
    });

    if (setupResult != kResultOk) {
        owner->getLogger()->logError("Failed to setupProcessing() for vst3. Result: %d", setupResult);
        return StatusCode::FAILED_TO_START_PROCESSING;
    }

    if (!attemptedActivation || activationResult != kResultOk) {
        owner->getLogger()->logError("Failed to setActive(true) for vst3 processing. Result: %d", activationResult);
        EventLoop::runTaskOnMainThread([&] {
            component->setActive(false);
        });
        return StatusCode::FAILED_TO_START_PROCESSING;
    }

    auto result = processor->setProcessing(true);
    // Surprisingly?, some VST3 plugins do not implement this function.
    // We do not prevent them just because of this.
    if (result != kResultOk && result != kNotImplemented) {
        owner->getLogger()->logError("Failed to start vst3 processing. Result: %d", result);
        EventLoop::runTaskOnMainThread([&] {
            component->setActive(false);
        });
        return StatusCode::FAILED_TO_START_PROCESSING;
    }

    owner->getLogger()->logInfo("%s: startProcessing() success", pluginName.c_str());
    owner->getHost()->startProcessing();
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::stopProcessing() {
    auto result = processor->setProcessing(false);
    // regarding kNotImplemented, see startProcessing().
    if (result != kResultOk && result != kNotImplemented) {
        owner->getLogger()->logError("Failed to stop vst3 processing. Result: %d", result);
        return StatusCode::FAILED_TO_STOP_PROCESSING;
    }

    tresult deactivateResult = kResultOk;
    EventLoop::runTaskOnMainThread([&] {
        deactivateResult = component->setActive(false);
    });
    if (deactivateResult != kResultOk)
        owner->getLogger()->logWarning("Failed to setActive(false) for vst3 processing. Result: %d", deactivateResult);

    // we deallocate memory where necessary.
    owner->getHost()->stopProcessing();

    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::process(AudioProcessContext &process) {
    // update audio buffer pointers
    const int32_t numFrames = process.frameCount();
    const int32_t numInputBus = processData.numInputs;
    const int32_t numOutputBus = processData.numOutputs;
    for (int32_t bus = 0, nBus = process.audioInBusCount(); bus < nBus; bus++) {
        if (bus >= numInputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more input buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numInputBus);
            continue;
        }
        for (size_t ch = 0, n = process.inputChannelCount(bus); ch < n; ch++) {
            if (processData.symbolicSampleSize == kSample32)
                processData.inputs[bus].channelBuffers32[ch] = process.getFloatInBuffer(bus, ch);
            else
                processData.inputs[bus].channelBuffers64[ch] = process.getDoubleInBuffer(bus, ch);
        }
    }
    for (int32_t bus = 0, nBus = process.audioOutBusCount(); bus < nBus; bus++) {
        if (bus >= numOutputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more output buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numOutputBus);
            continue;
        }
        for (size_t ch = 0, n = process.outputChannelCount(bus); ch < n; ch++) {
            if (processData.symbolicSampleSize == kSample32)
                processData.outputs[bus].channelBuffers32[ch] = process.getFloatOutBuffer(bus, ch);
            else
                processData.outputs[bus].channelBuffers64[ch] = process.getDoubleOutBuffer(bus, ch);
        }
    }

    const auto &ctx = processData.processContext;

    processData.numSamples = numFrames;

    // handle UMP inputs via UmpInputDispatcher.
    processDataInputEvents.clear();
    // FIXME: pass correct timestamp
    ump_input_dispatcher.process(0, process);

    processDataOutputParameterChanges.clear();

    // invoke plugin process
    auto result = processor->process(processData);

    processDataInputParameterChanges.clear();

    if (result != kResultOk) {
        owner->getLogger()->logError("Failed to process vst3 audio. Result: %d", result);
        return StatusCode::FAILED_TO_PROCESS;
    }

    // post-processing
    ctx->continousTimeSamples += numFrames;

    // FiXME: generate UMP outputs here
    processDataOutputEvents.clear();

    return StatusCode::OK;
}

remidy::PluginParameterSupport* remidy::PluginInstanceVST3::parameters() {
    if (!_parameters)
        _parameters = new ParameterSupport(this);
    return _parameters;
}

remidy::PluginStateSupport *remidy::PluginInstanceVST3::states() {
    if (!_states)
        _states = new PluginStatesVST3(this);
    return _states;
}

remidy::PluginPresetsSupport *remidy::PluginInstanceVST3::presets() {
    if (!_presets)
        _presets = new PresetsSupport(this);
    return _presets;
}

remidy::PluginUISupport *remidy::PluginInstanceVST3::ui() {
    if (!_ui)
        _ui = new UISupport(this);
    return _ui;
}
