
#include <iostream>
#include <algorithm>
#include <vector>

#include "remidy.hpp"
#include "../utils.hpp"
#include <algorithm>
#include <umppi/umppi.hpp>

#include "PluginFormatVST3.hpp"

using namespace remidy_vst3;

// FIXME: we should make edit controller lazily loaded.
//  Some plugins take long time to instantiate IEditController, and it does not make sense for
//  non-UI-based audio processing like our virtual MIDI devices.
remidy::PluginInstanceVST3::PluginInstanceVST3(
        PluginFormatVST3Impl *owner,
        PluginCatalogEntry *info,
        void *module,
        IPluginFactory *factory,
        ComponentHandlerImpl *handler,
        IComponent *component,
        IAudioProcessor *processor,
        IEditController *controller,
        bool isControllerDistinctFromComponent,
        FUnknown *instance
) : PluginInstance(info), owner(owner), module(module), factory(factory),
    component(component), processor(processor), controller(controller),
    isControllerDistinctFromComponent(isControllerDistinctFromComponent), instance(instance) {

    pluginName = info->displayName();

    auto result = controller->setComponentHandler(handler);
    if (result != kResultOk && result != kNoInterface)
        owner->getLogger()->logError(
                "%s: IEditController failed to set IComponentHandler. Result: %d",
                pluginName.c_str(), result);
    // set up IConnectionPoints
    result = component->queryInterface(IConnectionPoint::iid, (void **) &connPointComp);
    if (result != kResultOk && result != kNoInterface)
        owner->getLogger()->logError(
                "%s: IComponent failed to return query for IConnectionPoint as expected. Result: %d",
                pluginName.c_str(), result);
    result = controller->queryInterface(IConnectionPoint::iid, (void **) &connPointEdit);
    if (result != kResultOk && result != kNoInterface)
        owner->getLogger()->logError(
                "%s: IEditController failed to return query for IConnectionPoint as expected. Result: %d",
                pluginName.c_str(), result);

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

    audio_buses = new AudioBuses(this);

    // find NoteExpressionController
    if (controller->queryInterface(INoteExpressionController::iid, (void**) &note_expression_controller) != kResultOk)
        note_expression_controller = nullptr; // just to make sure
    if (controller->queryInterface(IUnitInfo::iid, (void**) &unit_info) != kResultOk)
        unit_info = nullptr; // just to make sure
    if (component->queryInterface(IProgramListData::iid, (void**) &program_list_data) != kResultOk)
        program_list_data = nullptr; // just to make sure
    // Try IMidiMapping2 first (VST3.8.0+), fallback to IMidiMapping
    if (controller->queryInterface(IMidiMapping2::iid, (void**) &midi_mapping2) != kResultOk)
        midi_mapping2 = nullptr; // just to make sure
    if (controller->queryInterface(IMidiMapping::iid, (void**) &midi_mapping) != kResultOk)
        midi_mapping = nullptr; // just to make sure

    // Register parameter edit handler for this plugin instance
    handler->setParameterEditHandler([this, controller](ParamID paramId, double value) {
        // Queue the parameter change for the next audio process call
        auto pvc = processDataInputParameterChanges.asInterface();
        int32_t index = 0;
        auto queue = pvc->addParameterData(paramId, index);
        if (queue) {
            int32_t pointIndex = 0;
            queue->addPoint(0, value, pointIndex);
        }

        double plainValue = controller->normalizedParamToPlain(paramId, value);
        dynamic_cast<ParameterSupport*>(parameters())->notifyParameterValue(paramId, plainValue);
    });

    handler->setRestartComponentHandler([this](int32 flags) {
        handleRestartComponent(flags);
    });

    // Leave the component inactive until startProcessing() explicitly activates it.

    synchronizeControllerState();
    refreshMidiMappings();
}

void remidy::PluginInstanceVST3::refreshMidiMappings() {
    // This must be called on the UI thread (or main thread)
    // Cache MIDI mappings for RT-safe access in audio processing

    cached_midi1_mappings_from_mapping2.clear();
    cached_midi2_mappings_from_mapping2.clear();
    cached_midi1_mappings_from_mapping.clear();

    // Cache from IMidiMapping2 (VST3.8.0+) if available
    if (midi_mapping2) {
        // Cache MIDI 1.0 controller mappings
        uint32_t count1 = midi_mapping2->getNumMidi1ControllerAssignments(kInput);
        if (count1 > 0) {
            cached_midi1_mappings_from_mapping2.resize(count1);
            Midi1ControllerParamIDAssignmentList list{count1, cached_midi1_mappings_from_mapping2.data()};
            if (midi_mapping2->getMidi1ControllerAssignments(kInput, list) != kResultOk) {
                cached_midi1_mappings_from_mapping2.clear();
            }
        }

        // Cache MIDI 2.0 controller mappings
        uint32_t count2 = midi_mapping2->getNumMidi2ControllerAssignments(kInput);
        if (count2 > 0) {
            cached_midi2_mappings_from_mapping2.resize(count2);
            Midi2ControllerParamIDAssignmentList list{count2, cached_midi2_mappings_from_mapping2.data()};
            if (midi_mapping2->getMidi2ControllerAssignments(kInput, list) != kResultOk) {
                cached_midi2_mappings_from_mapping2.clear();
            }
        }
    }

    // ALSO cache from IMidiMapping (pre-VST3.8.0) if available
    // This is NOT an "else" - we cache from BOTH interfaces
    if (midi_mapping) {
        // IMidiMapping uses a query-based API, so we need to query common controllers
        // Query for all standard MIDI 1.0 CC numbers (0-127) plus special controllers
        std::vector<CtrlNumber> controllersToQuery = {
            ControllerNumbers::kAfterTouch,
            ControllerNumbers::kPitchBend
        };
        // Add standard CC 0-127
        for (CtrlNumber cc = 0; cc < 128; cc++) {
            controllersToQuery.push_back(cc);
        }

        // Query mappings for each bus/channel/controller combination
        // We'll query for bus 0 and channels 0-15 (16 MIDI channels)
        for (int32 busIndex = 0; busIndex < 1; busIndex++) { // Typically only bus 0 for input
            for (int16 channel = 0; channel < 16; channel++) {
                for (CtrlNumber ctrlNum : controllersToQuery) {
                    ParamID paramId;
                    if (midi_mapping->getMidiControllerAssignment(busIndex, channel, ctrlNum, paramId) == kResultOk) {
                        // Successfully got a mapping from IMidiMapping
                        Midi1ControllerParamIDAssignment assignment;
                        assignment.pId = paramId;
                        assignment.busIndex = busIndex;
                        assignment.channel = channel;
                        assignment.controller = ctrlNum;
                        cached_midi1_mappings_from_mapping.push_back(assignment);
                    }
                }
            }
        }
    }
}

remidy::PluginInstanceVST3::~PluginInstanceVST3() {
    auto logger = owner->getLogger();

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
    const int32_t hostInputBusCount = process.audioInBusCount();
    for (int32_t bus = 0; bus < numInputBus; bus++) {
        auto available = bus < hostInputBusCount ? process.inputChannelCount(bus) : 0;
        auto pluginChannels = processData.inputs[bus].numChannels;
        for (int32_t ch = 0; ch < pluginChannels; ch++) {
            if (processData.symbolicSampleSize == kSample32) {
                float* ptr = nullptr;
                if (bus < hostInputBusCount && ch < static_cast<int32_t>(available))
                    ptr = process.getFloatInBuffer(bus, static_cast<uint32_t>(ch));
                else if (bus < hostInputBusCount && available > 0)
                    ptr = process.getFloatInBuffer(bus, static_cast<uint32_t>(0));
                if (!ptr)
                    ptr = ensureFloatBuffer(fallbackInput32);
                processData.inputs[bus].channelBuffers32[ch] = ptr;
            } else {
                double* ptr = nullptr;
                if (bus < hostInputBusCount && ch < static_cast<int32_t>(available))
                    ptr = process.getDoubleInBuffer(bus, static_cast<uint32_t>(ch));
                else if (bus < hostInputBusCount && available > 0)
                    ptr = process.getDoubleInBuffer(bus, static_cast<uint32_t>(0));
                if (!ptr)
                    ptr = ensureDoubleBuffer(fallbackInput64);
                processData.inputs[bus].channelBuffers64[ch] = ptr;
            }
        }
    }
    const int32_t hostOutputBusCount = process.audioOutBusCount();
    for (int32_t bus = 0; bus < numOutputBus; bus++) {
        auto available = bus < hostOutputBusCount ? process.outputChannelCount(bus) : 0;
        auto pluginChannels = processData.outputs[bus].numChannels;
        for (int32_t ch = 0; ch < pluginChannels; ch++) {
            if (processData.symbolicSampleSize == kSample32) {
                float* ptr = nullptr;
                if (bus < hostOutputBusCount && ch < static_cast<int32_t>(available))
                    ptr = process.getFloatOutBuffer(bus, static_cast<uint32_t>(ch));
                if (!ptr)
                    ptr = ensureFloatBuffer(fallbackOutput32);
                processData.outputs[bus].channelBuffers32[ch] = ptr;
            } else {
                double* ptr = nullptr;
                if (bus < hostOutputBusCount && ch < static_cast<int32_t>(available))
                    ptr = process.getDoubleOutBuffer(bus, static_cast<uint32_t>(ch));
                if (!ptr)
                    ptr = ensureDoubleBuffer(fallbackOutput64);
                processData.outputs[bus].channelBuffers64[ch] = ptr;
            }
        }
    }

    const auto &ctx = processData.processContext;

    // Update ProcessContext with transport info from MasterContext
    auto& masterContext = process.masterContext();

    process_context.projectTimeSamples = masterContext.playbackPositionSamples();
    process_context.continousTimeSamples = masterContext.playbackPositionSamples();
    process_context.sampleRate = masterContext.sampleRate();
    processData.symbolicSampleSize = process.masterContext().audioDataType() == AudioContentType::Float64 ? kSample64 : kSample32;

    // Update state flags
    uint32_t state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
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

    // Time signature from MasterContext
    process_context.timeSigNumerator = masterContext.timeSignatureNumerator();
    process_context.timeSigDenominator = masterContext.timeSignatureDenominator();

    processData.numSamples = numFrames;

    // handle UMP inputs via UmpInputDispatcher.
    processDataInputEvents.clear();
    ump_input_dispatcher.process(process);

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
    auto* parameterSupport = dynamic_cast<PluginInstanceVST3::ParameterSupport*>(parameters());

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
                    uint64_t ump = umppi::UmpFactory::midi2NoteOn(
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
                    uint64_t ump = umppi::UmpFactory::midi2NoteOff(
                        group, e.noteOff.channel, e.noteOff.pitch,
                        attributeType, velocity, attributeData);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case Event::kPolyPressureEvent: {
                    uint32_t pressure = static_cast<uint32_t>(e.polyPressure.pressure * 4294967295.0f);
                    uint64_t ump = umppi::UmpFactory::midi2PAf(
                        group, e.polyPressure.channel, e.polyPressure.pitch, pressure);
                    umpBuffer[umpPosition++] = (uint32_t)(ump >> 32);
                    umpBuffer[umpPosition++] = (uint32_t)(ump & 0xFFFFFFFF);
                    break;
                }
                case Event::kNoteExpressionValueEvent: {
                    if (!parameterSupport)
                        break;
                    auto maybeIndex = parameterSupport->indexForNoteExpressionType(static_cast<uint32_t>(e.busIndex),
                                                                                    0, // FIXME: is there any correct value?
                                                                                    e.noteExpressionValue.typeId);
                    if (!maybeIndex.has_value())
                        break;
                    const uint32_t controllerIndex = maybeIndex.value();
                    const double plainValue = e.noteExpressionValue.value;
                    parameterSupport->notifyPerNoteControllerValue(PER_NOTE_CONTROLLER_PER_NOTE,
                                                                   static_cast<uint32_t>(e.noteExpressionValue.noteId),
                                                                   controllerIndex,
                                                                   plainValue);
                    const double clamped = std::clamp(plainValue, 0.0, 1.0);
                    const uint32_t data = static_cast<uint32_t>(clamped * 4294967295.0);
                    uint64_t ump = umppi::UmpFactory::midi2PerNoteACC(
                        group,
                        0, // FIXME: is there any correct value?
                        static_cast<uint8_t>(e.noteExpressionValue.noteId),
                        static_cast<uint8_t>(controllerIndex & 0x7F),
                        data);
                    umpBuffer[umpPosition++] = static_cast<uint32_t>(ump >> 32);
                    umpBuffer[umpPosition++] = static_cast<uint32_t>(ump & 0xFFFFFFFF);
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
                        ump = umppi::UmpFactory::midi2PitchBendDirect(group, channel, pitchValue << 18);
                    } else if (cc == 129) { // kCtrlPolyPressure
                        uint32_t pressure = static_cast<uint32_t>(e.midiCCOut.value2) << 25;
                        ump = umppi::UmpFactory::midi2PAf(group, channel, e.midiCCOut.value, pressure);
                    } else {
                        // Regular CC
                        ump = umppi::UmpFactory::midi2CC(group, channel, cc, value);
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
                    dynamic_cast<PluginInstanceVST3::ParameterSupport*>(parameters())->notifyParameterValue(paramId, plainValue);
                    // Convert parameter to MIDI 2.0 AC (Assignable Controller) using NRPN
                    // AC uses bank (MSB) and index (LSB): paramId = bank * 128 + index
                    uint8_t bank = static_cast<uint8_t>((paramId >> 7) & 0x7F);
                    uint8_t index = static_cast<uint8_t>(paramId & 0x7F);
                    uint32_t data = static_cast<uint32_t>(value * 4294967295.0);

                    // Use NRPN for channel-wide assignable controllers
                    uint64_t ump = umppi::UmpFactory::midi2NRPN(0, 0, bank, index, data); // group 0, channel 0
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
            // MIDI CC mapping changed - refresh cached mappings
            logger->logInfo("%s: Handling kMidiCCAssignmentChanged - refreshing MIDI mappings", pluginName.c_str());
            refreshMidiMappings();
        }

        if (flags & Vst::RestartFlags::kParamValuesChanged) {
            // Parameter values changed - re-read all parameter values
            logger->logInfo("%s: Handling kParamValuesChanged - refreshing parameter values", pluginName.c_str());
            // Re-query all parameter values from the controller and notify listeners
            auto paramSupport = dynamic_cast<PluginInstanceVST3::ParameterSupport*>(parameters());
            if (paramSupport) {
                auto& params = paramSupport->parameters();
                for (size_t i = 0; i < params.size(); i++) {
                    double value;
                    if (paramSupport->getParameter(static_cast<uint32_t>(i), &value) == StatusCode::OK) {
                        // Notify listeners about the parameter value change
                        auto paramId = paramSupport->getParameterId(static_cast<uint32_t>(i));
                        paramSupport->notifyParameterValue(paramId, value);
                    }
                }
            }
        }

        if (flags & Vst::RestartFlags::kParamTitlesChanged) {
            // Parameter metadata changed - re-read parameter info
            logger->logInfo("%s: Handling kParamTitlesChanged - refreshing parameter metadata", pluginName.c_str());
            // At minimum, refresh parameter metadata (min/max/default values)
            auto paramSupport = dynamic_cast<PluginInstanceVST3::ParameterSupport*>(parameters());
            if (paramSupport) {
                paramSupport->refreshAllParameterMetadata();
            }
            // NOTE: Parameter names/titles are stored as const strings, so a full implementation
            // would require recreating the parameter support to pick up new names.
            logger->logWarning("%s: kParamTitlesChanged - parameter names may not be updated (requires recreation)",
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
            logger->logInfo("%s: Handling kParamIDMappingChanged - rebuilding parameter list", pluginName.c_str());
            auto paramSupport = dynamic_cast<PluginInstanceVST3::ParameterSupport*>(parameters());
            if (paramSupport)
                paramSupport->refreshAllParameterMetadata();
        }
    });
}

void remidy::PluginInstanceVST3::synchronizeControllerState() {
    if (!component || !controller)
        return;

    EventLoop::runTaskOnMainThread([&] {
        std::vector<uint8_t> componentState;
        VectorStream componentStream(componentState);
        auto result = component->getState((IBStream*) &componentStream);
        if (result != kResultOk)
            return;

        VectorStream controllerStream(componentState);
        controller->setComponentState((IBStream*) &controllerStream);
    });
}

// ComponentHandlerImpl
tresult PLUGIN_API remidy::ComponentHandlerImpl::queryInterface(const TUID _iid, void** obj) {
    QUERY_INTERFACE(_iid, obj, FUnknown::iid, IComponentHandler)
    QUERY_INTERFACE(_iid, obj, IComponentHandler::iid, IComponentHandler)
    QUERY_INTERFACE(_iid, obj, IComponentHandler2::iid, IComponentHandler2)
    QUERY_INTERFACE(_iid, obj, IUnitHandler::iid, IUnitHandler)
    // Delegate IRunLoop to the host-global HostApplication
    if (FUnknownPrivate::iidEqual(_iid, IRunLoop::iid)) {
        auto* rl = host ? host->getRunLoop() : nullptr;
        if (rl) rl->addRef();
        *obj = rl;
        return rl ? kResultOk : kNoInterface;
    }
    logNoInterface("IComponentHandler::queryInterface", _iid);
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::beginEdit(ParamID id) {
    // Begin parameter edit - currently not implemented
    remidy::Logger::global()->logWarning("ComponentHandler::beginEdit invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::performEdit(ParamID id, ParamValue valueNormalized) {
    // Find and invoke parameter edit handler if registered
    if (parameter_edit_handler)
        parameter_edit_handler(id, valueNormalized);
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::endEdit(ParamID id) {
    // End parameter edit - currently not implemented
    remidy::Logger::global()->logWarning("ComponentHandler::endEdit invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::restartComponent(int32 flags) {
    if (flags == 0)
        return kResultOk; // Nothing to do
    Logger::global()->logInfo("ComponentHandler::restartComponent invoked with flags: 0x%x", flags);
    if (restart_component_handler)
        restart_component_handler(flags);
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::setDirty(TBool state) {
    remidy::Logger::global()->logWarning("ComponentHandler2::setDirty invoked (not implemented in this host)");
    // Set dirty state - currently not implemented
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::requestOpenEditor(FIDString name) {
    // Request open editor - currently not implemented
    remidy::Logger::global()->logWarning("ComponentHandler2::requestOpenEditor invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::startGroupEdit() {
    // Start group edit - currently not implemented
    remidy::Logger::global()->logWarning("ComponentHandler2::startGroupEdit invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::finishGroupEdit() {
    // Finish group edit - currently not implemented
    remidy::Logger::global()->logWarning("ComponentHandler2::finishGroupEdit invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::notifyUnitSelection(UnitID unitId) {
    // Notify unit selection - currently not implemented
    remidy::Logger::global()->logWarning("IUnitHandler::notifyUnitSelection invoked (not implemented in this host)");
    return kResultOk;
}

tresult PLUGIN_API remidy::ComponentHandlerImpl::notifyProgramListChange(ProgramListID listId, int32 programIndex) {
    // Notify program list change - currently not implemented
    remidy::Logger::global()->logWarning("IUnitHandler::notifyProgramListChange invoked (not implemented in this host)");
    return kResultOk;
}
