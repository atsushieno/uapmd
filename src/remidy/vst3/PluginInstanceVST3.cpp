
#include <iostream>
#include <algorithm>
#include <vector>

#include "remidy.hpp"
#include "../utils.hpp"
#include "cmidi2.h"

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
    owner->getHost()->setParameterEditHandler(controller, [this, controller](ParamID paramId, double value) {
        // Queue the parameter change for the next audio process call
        auto pvc = processDataInputParameterChanges.asInterface();
        int32_t index = 0;
        auto queue = pvc->addParameterData(paramId, index);
        if (queue) {
            int32_t pointIndex = 0;
            queue->addPoint(0, value, pointIndex);
        }

        if (_parameters) {
            double plainValue = controller->normalizedParamToPlain(paramId, value);
            _parameters->notifyParameterValue(paramId, plainValue);
        }
    });

    owner->getHost()->setRestartComponentHandler(controller, [this](int32 flags) {
        handleRestartComponent(flags);
    });

    // Leave the component inactive until startProcessing() explicitly activates it.
}

remidy::PluginInstanceVST3::~PluginInstanceVST3() {
    auto logger = owner->getLogger();

    // Unregister parameter edit handler
    owner->getHost()->setParameterEditHandler(controller, nullptr);

    // Unregister restart component handler
    owner->getHost()->setRestartComponentHandler(controller, nullptr);

    auto result = processor->setProcessing(false);
    if (result == kResultOk)
        processingActive = false;
    if (result != kResultOk)
        logger->logError("Failed to setProcessing(false) at VST3 destructor: %d", result);
    EventLoop::runTaskOnMainThread([this, &result, logger] {
        result = component->setActive(false);
        if (result == kResultOk)
            componentActive = false;
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
            componentActive = activationResult == kResultOk;
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
            componentActive = false;
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
            componentActive = false;
        });
        return StatusCode::FAILED_TO_START_PROCESSING;
    }
    processingActive = result == kResultOk;

    owner->getLogger()->logInfo("%s: startProcessing() success", pluginName.c_str());
    owner->getHost()->startProcessing();
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceVST3::stopProcessing() {
    auto result = processor->setProcessing(false);
    if (result == kResultOk)
        processingActive = false;
    // regarding kNotImplemented, see startProcessing().
    if (result != kResultOk && result != kNotImplemented) {
        owner->getLogger()->logError("Failed to stop vst3 processing. Result: %d", result);
        return StatusCode::FAILED_TO_STOP_PROCESSING;
    }

    tresult deactivateResult = kResultOk;
    EventLoop::runTaskOnMainThread([&] {
        deactivateResult = component->setActive(false);
        if (deactivateResult == kResultOk)
            componentActive = false;
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
    thread_local std::vector<float> fallbackInput32;
    thread_local std::vector<double> fallbackInput64;
    thread_local std::vector<float> fallbackOutput32;
    thread_local std::vector<double> fallbackOutput64;

    auto ensureFloatBuffer = [&](std::vector<float>& buffer) -> float* {
        if (buffer.size() < static_cast<size_t>(numFrames))
            buffer.resize(static_cast<size_t>(numFrames), 0.0f);
        else
            std::fill(buffer.begin(), buffer.begin() + numFrames, 0.0f);
        return buffer.data();
    };
    auto ensureDoubleBuffer = [&](std::vector<double>& buffer) -> double* {
        if (buffer.size() < static_cast<size_t>(numFrames))
            buffer.resize(static_cast<size_t>(numFrames), 0.0);
        else
            std::fill(buffer.begin(), buffer.begin() + numFrames, 0.0);
        return buffer.data();
    };
    for (int32_t bus = 0, nBus = process.audioInBusCount(); bus < nBus; bus++) {
        if (bus >= numInputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more input buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numInputBus);
            continue;
        }
        auto available = process.inputChannelCount(bus);
        auto pluginChannels = processData.inputs[bus].numChannels;
        for (int32_t ch = 0; ch < pluginChannels; ch++) {
            if (processData.symbolicSampleSize == kSample32) {
                float* ptr = nullptr;
                if (ch < static_cast<int32_t>(available))
                    ptr = process.getFloatInBuffer(bus, static_cast<uint32_t>(ch));
                else if (available > 0)
                    ptr = process.getFloatInBuffer(bus, static_cast<uint32_t>(0));
                if (!ptr)
                    ptr = ensureFloatBuffer(fallbackInput32);
                processData.inputs[bus].channelBuffers32[ch] = ptr;
            } else {
                double* ptr = nullptr;
                if (ch < static_cast<int32_t>(available))
                    ptr = process.getDoubleInBuffer(bus, static_cast<uint32_t>(ch));
                else if (available > 0)
                    ptr = process.getDoubleInBuffer(bus, static_cast<uint32_t>(0));
                if (!ptr)
                    ptr = ensureDoubleBuffer(fallbackInput64);
                processData.inputs[bus].channelBuffers64[ch] = ptr;
            }
        }
    }
    for (int32_t bus = 0, nBus = process.audioOutBusCount(); bus < nBus; bus++) {
        if (bus >= numOutputBus) {
            // disabed the log, too noisy.
            //owner->getLogger()->logError("The process context has more output buses (%d) than the plugin supports (%d). Ignoring them.", nBus, numOutputBus);
            continue;
        }
        auto available = process.outputChannelCount(bus);
        auto pluginChannels = processData.outputs[bus].numChannels;
        for (int32_t ch = 0; ch < pluginChannels; ch++) {
            if (processData.symbolicSampleSize == kSample32) {
                float* ptr = nullptr;
                if (ch < static_cast<int32_t>(available))
                    ptr = process.getFloatOutBuffer(bus, static_cast<uint32_t>(ch));
                if (!ptr)
                    ptr = ensureFloatBuffer(fallbackOutput32);
                processData.outputs[bus].channelBuffers32[ch] = ptr;
            } else {
                double* ptr = nullptr;
                if (ch < static_cast<int32_t>(available))
                    ptr = process.getDoubleOutBuffer(bus, static_cast<uint32_t>(ch));
                if (!ptr)
                    ptr = ensureDoubleBuffer(fallbackOutput64);
                processData.outputs[bus].channelBuffers64[ch] = ptr;
            }
        }
    }

    const auto &ctx = processData.processContext;

    // Update ProcessContext with transport info from MasterContext
    auto* trackContext = process.trackContext();
    auto& masterContext = trackContext->masterContext();

    process_context.projectTimeSamples = masterContext.playbackPositionSamples();
    process_context.continousTimeSamples = masterContext.playbackPositionSamples();
    process_context.sampleRate = masterContext.sampleRate();

    // Update state flags
    uint32_t state = 0;
    if (masterContext.isPlaying()) {
        state |= ProcessContext::kPlaying;
    }
    process_context.state = state;

    // Calculate PPQ position from samples
    // PPQ = (samples / sampleRate) * (tempo_bpm / 60)
    // tempo in VST3 is in BPM (beats per minute), masterContext.tempo() is in microseconds per quarter note
    double tempoBPM = 60000000.0 / masterContext.tempo();
    double seconds = static_cast<double>(masterContext.playbackPositionSamples()) / masterContext.sampleRate();
    process_context.projectTimeMusic = (seconds * tempoBPM) / 60.0;
    process_context.tempo = tempoBPM;

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

    // Convert VST3 output events to UMP
    auto& eventOut = process.eventOut();
    auto* umpBuffer = static_cast<uint32_t*>(eventOut.getMessages());
    size_t umpPosition = eventOut.position() / sizeof(uint32_t); // position in uint32_t units
    size_t umpCapacity = eventOut.maxMessagesInBytes() / sizeof(uint32_t);

    // Process output events (notes, poly pressure, etc.)
    int32_t eventCount = processDataOutputEvents.getEventCount();
    for (int32_t i = 0; i < eventCount && umpPosition < umpCapacity; ++i) {
        Event e;
        if (processDataOutputEvents.getEvent(i, e) == kResultOk) {
            uint8_t group = static_cast<uint8_t>(e.busIndex);

            switch (e.type) {
                case Event::kNoteOnEvent: {
                    uint16_t velocity = static_cast<uint16_t>(e.noteOn.velocity * 65535.0f);
                    uint16_t attributeData = 0; // VST3 doesn't provide attribute data
                    uint8_t attributeType = 0;
                    uint64_t ump = cmidi2_ump_midi2_note_on(
                        group, e.noteOn.channel, e.noteOn.pitch,
                        attributeType, velocity, attributeData);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case Event::kNoteOffEvent: {
                    uint16_t velocity = static_cast<uint16_t>(e.noteOff.velocity * 65535.0f);
                    uint16_t attributeData = 0;
                    uint8_t attributeType = 0;
                    uint64_t ump = cmidi2_ump_midi2_note_off(
                        group, e.noteOff.channel, e.noteOff.pitch,
                        attributeType, velocity, attributeData);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case Event::kPolyPressureEvent: {
                    uint32_t pressure = static_cast<uint32_t>(e.polyPressure.pressure * 4294967295.0f);
                    uint64_t ump = cmidi2_ump_midi2_paf(
                        group, e.polyPressure.channel, e.polyPressure.pitch, pressure);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case Event::kLegacyMIDICCOutEvent: {
                    uint8_t channel = e.midiCCOut.channel >= 0 ? static_cast<uint8_t>(e.midiCCOut.channel) : 0;
                    uint8_t cc = e.midiCCOut.controlNumber;
                    uint32_t value = static_cast<uint32_t>(e.midiCCOut.value) << 25; // 7-bit to 32-bit
                    uint64_t ump;

                    // Special handling for pitch bend and poly pressure
                    if (cc == 128) { // kPitchBend
                        uint32_t pitchValue = (static_cast<uint32_t>(e.midiCCOut.value2) << 7) | e.midiCCOut.value;
                        ump = cmidi2_ump_midi2_pitch_bend_direct(group, channel, pitchValue << 18);
                    } else if (cc == 129) { // kCtrlPolyPressure
                        uint32_t pressure = static_cast<uint32_t>(e.midiCCOut.value2) << 25;
                        ump = cmidi2_ump_midi2_paf(group, channel, e.midiCCOut.value, pressure);
                    } else {
                        // Regular CC
                        ump = cmidi2_ump_midi2_cc(group, channel, cc, value);
                    }
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                default:
                    // Other event types not yet supported
                    break;
            }
        }
    }

    // Process output parameter changes
    int32_t paramCount = processDataOutputParameterChanges.getParameterCount();
    for (int32_t i = 0; i < paramCount && umpPosition < umpCapacity; ++i) {
        auto* queue = processDataOutputParameterChanges.getParameterData(i);
        if (queue) {
            ParamID paramId = queue->getParameterId();
            int32_t pointCount = queue->getPointCount();

            // For now, just take the last value in the queue
            if (pointCount > 0) {
                int32_t sampleOffset;
                ParamValue value;
                if (queue->getPoint(pointCount - 1, sampleOffset, value) == kResultOk) {
                    double plainValue = controller->normalizedParamToPlain(paramId, value);
                    if (_parameters)
                        _parameters->notifyParameterValue(paramId, plainValue);
                    // Convert parameter to MIDI 2.0 AC (Assignable Controller) using NRPN
                    // AC uses bank (MSB) and index (LSB): paramId = bank * 128 + index
                    uint8_t bank = static_cast<uint8_t>((paramId >> 7) & 0x7F);
                    uint8_t index = static_cast<uint8_t>(paramId & 0x7F);
                    uint32_t data = static_cast<uint32_t>(value * 4294967295.0);

                    // Use NRPN for channel-wide assignable controllers
                    uint64_t ump = cmidi2_ump_midi2_nrpn(0, 0, bank, index, data); // group 0, channel 0
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                }
            }
        }
    }

    // Update eventOut position
    eventOut.position(umpPosition * sizeof(uint32_t));

    processDataOutputEvents.clear();

    return StatusCode::OK;
}

void remidy::PluginInstanceVST3::setOfflineMode(bool offlineMode) {
    auto desiredMode = offlineMode ? kOffline : kRealtime;
    if (last_process_setup.processMode == desiredMode && processData.processMode == desiredMode)
        return;

    last_process_setup.processMode = desiredMode;
    processData.processMode = desiredMode;

    if (!has_process_setup || !processor || !component)
        return;

    auto logger = owner->getLogger();
    auto result = processor->setProcessing(false);
    if (result == kResultOk)
        processingActive = false;
    if (result != kResultOk && result != kNotImplemented)
        logger->logWarning("%s: setOfflineMode() could not stop processing. Result: %d", pluginName.c_str(), result);

    tresult deactivateResult = kResultOk;
    EventLoop::runTaskOnMainThread([&] {
        deactivateResult = component->setActive(false);
        if (deactivateResult == kResultOk)
            componentActive = false;
    });
    if (deactivateResult != kResultOk)
        logger->logWarning("%s: setOfflineMode() could not deactivate component. Result: %d", pluginName.c_str(), deactivateResult);

    tresult setupResult = kResultOk;
    EventLoop::runTaskOnMainThread([&] {
        setupResult = processor->setupProcessing(last_process_setup);
    });
    if (setupResult != kResultOk) {
        logger->logError("%s: setOfflineMode() failed to setup processing. Result: %d", pluginName.c_str(), setupResult);
    }

    tresult activateResult = kResultOk;
    EventLoop::runTaskOnMainThread([&] {
        activateResult = component->setActive(true);
        if (activateResult == kResultOk)
            componentActive = true;
    });
    if (activateResult != kResultOk)
        logger->logWarning("%s: setOfflineMode() could not activate component. Result: %d", pluginName.c_str(), activateResult);

    result = processor->setProcessing(true);
    if (result != kResultOk && result != kNotImplemented)
        logger->logWarning("%s: setOfflineMode() could not restart processing. Result: %d", pluginName.c_str(), result);
    else if (result == kResultOk)
        processingActive = true;
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

void remidy::PluginInstanceVST3::handleRestartComponent(int32 flags) {
    auto logger = owner->getLogger();

    remidy::EventLoop::runTaskOnMainThread([this, flags, logger] {
        if (flags & Vst::RestartFlags::kReloadComponent) {
            // Full component reload - reset the plugin state
            logger->logInfo("%s: Handling kReloadComponent - resetting plugin (not fully implemented)", pluginName.c_str());
            // Note: We don't fully reload/unload the plugin, just reset it
            // A full reload would require recreating the plugin instance
            if (component) {
                // Reset the component state
                auto result = component->setActive(false);
                if (result == kResultOk) {
                    componentActive = false;
                    result = component->setActive(true);
                    if (result != kResultOk) {
                        logger->logError("%s: Failed to reactivate component after reset: %d",
                                       pluginName.c_str(), result);
                    } else {
                        componentActive = true;
                    }
                } else {
                    logger->logError("%s: Failed to deactivate component for reset: %d",
                                   pluginName.c_str(), result);
                }
            }
        }

        if (flags & Vst::RestartFlags::kIoChanged) {
            // Bus configuration changed - need to deactivate and reactivate
            logger->logInfo("%s: Handling kIoChanged - reconfiguring I/O", pluginName.c_str());

            if (has_process_setup) {
                // Deactivate
                auto deactivateResult = component->setActive(false);
                if (deactivateResult != kResultOk) {
                    logger->logWarning("%s: Failed to deactivate for I/O change: %d",
                                     pluginName.c_str(), deactivateResult);
                } else {
                    componentActive = false;
                }

                // Re-inspect buses to pick up new configuration
                audio_buses->inspectBuses();

                // Reactivate with current setup
                auto setupResult = processor->setupProcessing(last_process_setup);
                if (setupResult == kResultOk) {
                    auto activateResult = component->setActive(true);
                    if (activateResult != kResultOk) {
                        logger->logError("%s: Failed to reactivate after I/O change: %d",
                                       pluginName.c_str(), activateResult);
                    } else {
                        componentActive = true;
                    }
                } else {
                    logger->logError("%s: Failed to setup processing after I/O change: %d",
                                   pluginName.c_str(), setupResult);
                }
            }
        }

        if (flags & Vst::RestartFlags::kLatencyChanged) {
            // Latency changed - query new latency
            logger->logInfo("%s: Handling kLatencyChanged - querying new latency (not implemented)", pluginName.c_str());
            if (processor) {
                uint32 newLatency = processor->getLatencySamples();
                logger->logInfo("%s: New latency: %u samples", pluginName.c_str(), newLatency);
                // TODO: Update host's delay compensation if needed
            }
        }

        if (flags & Vst::RestartFlags::kMidiCCAssignmentChanged) {
            // MIDI CC mapping changed
            logger->logInfo("%s: Handling kMidiCCAssignmentChanged - MIDI mapping updated (not implemented)", pluginName.c_str());
            // TODO: Re-query MIDI mappings if we use them
        }

        if (flags & Vst::RestartFlags::kParamValuesChanged) {
            // Parameter values changed - re-read all parameter values
            logger->logInfo("%s: Handling kParamValuesChanged - refreshing parameter values (not fully implemented)", pluginName.c_str());
            if (_parameters) {
                // Re-query all parameter values from the controller
                auto& params = _parameters->parameters();
                for (size_t i = 0; i < params.size(); i++) {
                    double value;
                    if (_parameters->getParameter(i, &value) == StatusCode::OK) {
                        // Value has been updated internally
                    }
                }
            }
        }

        if (flags & Vst::RestartFlags::kParamTitlesChanged) {
            // Parameter metadata changed - re-read parameter info
            logger->logInfo("%s: Handling kParamTitlesChanged - refreshing parameter info", pluginName.c_str());
            // The ParameterSupport would need to be re-initialized to pick up new parameter info
            // For now, just log it. A full implementation would recreate the parameter support.
            logger->logWarning("%s: kParamTitlesChanged requires recreating parameter support (not fully implemented)",
                             pluginName.c_str());
        }

        // Log warnings for unhandled flags
        if (flags & Vst::RestartFlags::kNoteExpressionChanged) {
            logger->logWarning("%s: kNoteExpressionChanged not yet implemented", pluginName.c_str());
        }
        if (flags & Vst::RestartFlags::kIoTitlesChanged) {
            logger->logWarning("%s: kIoTitlesChanged not yet implemented", pluginName.c_str());
        }
        if (flags & Vst::RestartFlags::kPrefetchableSupportChanged) {
            logger->logWarning("%s: kPrefetchableSupportChanged not yet implemented", pluginName.c_str());
        }
        if (flags & Vst::RestartFlags::kRoutingInfoChanged) {
            logger->logWarning("%s: kRoutingInfoChanged not yet implemented", pluginName.c_str());
        }
        if (flags & Vst::RestartFlags::kKeyswitchChanged) {
            logger->logWarning("%s: kKeyswitchChanged not yet implemented", pluginName.c_str());
        }
        if (flags & Vst::RestartFlags::kParamIDMappingChanged) {
            logger->logWarning("%s: kParamIDMappingChanged not yet implemented", pluginName.c_str());
        }
    });
}
