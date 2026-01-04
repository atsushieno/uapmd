#if __APPLE__

#include "PluginFormatAUv3.hpp"
#include "AUv2Helper.hpp"
#include "cmidi2.h"
#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <CoreMIDI/CoreMIDI.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdlib>

namespace {
    constexpr size_t kMidiEventListCapacityBytes = 65536;
}

remidy::PluginInstanceAUv3::PluginInstanceAUv3(
        PluginFormatAUv3Impl *format,
        PluginFormat::PluginInstantiationOptions options,
        Logger* logger,
        PluginCatalogEntry* info,
        AVAudioUnit* avAudioUnit,
        AUAudioUnit* audioUnit
) : PluginInstance(info), format(format), options(options), logger_(logger), avAudioUnit(avAudioUnit), audioUnit(audioUnit) {
    @autoreleasepool {
        name = std::string([[audioUnit componentName] UTF8String]);
        setCurrentThreadNameIfPossible("remidy.AUv3.instance." + name);
        if (@available(macOS 11.0, *)) {
            if ([audioUnit isKindOfClass:[AUAudioUnitV2Bridge class]]) {
                auto bridge = static_cast<AUAudioUnitV2Bridge*>(audioUnit);
                bridgedAudioUnit = bridge.audioUnit;
            }
        }
        audio_buses = new AudioBuses(this);
        midiConverter = new MIDIEventConverter();
        midi_event_list_capacity = kMidiEventListCapacityBytes;
        midi_event_list = static_cast<MIDIEventList*>(std::calloc(1, midi_event_list_capacity));
        initializeHostCallbacks();
    }
}

remidy::PluginInstanceAUv3::~PluginInstanceAUv3() {
    @autoreleasepool {
        auto releaseAudioObjects = [&] {
            if (audioUnit != nil) {
                [audioUnit release];
                audioUnit = nil;
            }
            if (avAudioUnit != nil) {
                [avAudioUnit release];
                avAudioUnit = nil;
            }
        };

        if (options.uiThreadRequirement & PluginUIThreadRequirement::InstanceControl)
            EventLoop::runTaskOnMainThread(releaseAudioObjects);
        else
            releaseAudioObjects();
        delete audio_buses;
        delete midiConverter;
        delete _parameters;
        delete _states;
        delete _presets;
        delete _ui;
        if (midi_event_list) {
            std::free(midi_event_list);
            midi_event_list = nullptr;
            midi_event_list_capacity = 0;
        }
    }
}

void remidy::PluginInstanceAUv3::initializeHostCallbacks() {
    @autoreleasepool {
        if (audioUnit == nil)
            return;

        host_transport_info = HostTransportInfo{};

        // Set up transport state block
        // NOTE: __weak doesn't work with C++ pointers, using __unsafe_unretained instead
        // This is safe because the blocks are owned by audioUnit which is owned by this instance
        __unsafe_unretained PluginInstanceAUv3* weakSelf = this;
        audioUnit.musicalContextBlock = ^BOOL(double * _Nullable outCurrentTempo,
                                              double * _Nullable outTimeSignatureNumerator,
                                              NSInteger * _Nullable outTimeSignatureDenominator,
                                              double * _Nullable outCurrentBeatPosition,
                                              NSInteger * _Nullable outSampleOffsetToNextBeat,
                                              double * _Nullable outCurrentMeasureDownbeatPosition) {
            PluginInstanceAUv3* strongSelf = weakSelf;
            if (!strongSelf)
                return NO;

            auto& info = strongSelf->host_transport_info;

            if (outCurrentTempo)
                *outCurrentTempo = info.currentTempo;
            if (outTimeSignatureNumerator)
                *outTimeSignatureNumerator = info.timeSigNumerator;
            if (outTimeSignatureDenominator)
                *outTimeSignatureDenominator = info.timeSigDenominator;
            if (outCurrentBeatPosition)
                *outCurrentBeatPosition = info.currentBeat;
            if (outSampleOffsetToNextBeat) {
                if (info.currentTempo > 0.0 && info.sampleRate > 0.0) {
                    double samplesPerBeat = info.sampleRate * 60.0 / info.currentTempo;
                    double beatFraction = std::fmod(info.currentBeat, 1.0);
                    double remainingBeats = (beatFraction > 0.0) ? (1.0 - beatFraction) : 0.0;
                    *outSampleOffsetToNextBeat = static_cast<NSInteger>(remainingBeats * samplesPerBeat);
                } else {
                    *outSampleOffsetToNextBeat = 0;
                }
            }
            if (outCurrentMeasureDownbeatPosition) {
                if (info.timeSigNumerator > 0) {
                    double measureIndex = std::floor(info.currentBeat / static_cast<double>(info.timeSigNumerator));
                    *outCurrentMeasureDownbeatPosition = measureIndex * static_cast<double>(info.timeSigNumerator);
                } else {
                    *outCurrentMeasureDownbeatPosition = 0.0;
                }
            }

            return YES;
        };

        audioUnit.transportStateBlock = ^BOOL(AUHostTransportStateFlags * _Nullable outTransportStateFlags,
                                              double * _Nullable outCurrentSamplePosition,
                                              double * _Nullable outCycleStartBeatPosition,
                                              double * _Nullable outCycleEndBeatPosition) {
            PluginInstanceAUv3* strongSelf = weakSelf;
            if (!strongSelf)
                return NO;

            auto& info = strongSelf->host_transport_info;

            if (outTransportStateFlags) {
                AUHostTransportStateFlags flags = 0;
                if (info.isPlaying)
                    // NOTE: some non-trivial replacement during AUv2->AUv3 migration
                    flags |= AUHostTransportStateMoving; // AUHostTransportStatePlaying not available in older SDKs
                if (info.isRecording)
                    flags |= AUHostTransportStateRecording;
                if (info.isCycling)
                    flags |= AUHostTransportStateCycling;
                if (info.transportStateChanged)
                    flags |= AUHostTransportStateChanged;
                *outTransportStateFlags = flags;
            }
            if (outCurrentSamplePosition)
                *outCurrentSamplePosition = info.currentSample;
            if (outCycleStartBeatPosition)
                *outCycleStartBeatPosition = info.cycleStart;
            if (outCycleEndBeatPosition)
                *outCycleEndBeatPosition = info.cycleEnd;

            return YES;
        };
    }
}

remidy::StatusCode remidy::PluginInstanceAUv3::configure(ConfigurationRequest& configuration) {
    @autoreleasepool {
        if (audioUnit == nil) {
            logger()->logError("%s: PluginInstanceAUv3::configure - audioUnit is nil", name.c_str());
            return StatusCode::FAILED_TO_CONFIGURE;
        }

        NSError* error = nil;

        // Configure sample rate and format
        host_transport_info.sampleRate = static_cast<double>(configuration.sampleRate);

        auto ret = audio_buses->configure(configuration);
        if (ret != StatusCode::OK)
            return ret;

        // Set maximum frames per slice
        audioUnit.maximumFramesToRender = configuration.bufferSizeInSamples;

        // Allocate render resources
        if (![audioUnit allocateRenderResourcesAndReturnError:&error]) {
            logger()->logError("%s: PluginInstanceAUv3::configure failed to allocate render resources: %s",
                             name.c_str(), [[error localizedDescription] UTF8String]);
            return StatusCode::FAILED_TO_CONFIGURE;
        }

        return StatusCode::OK;
    }
}

remidy::StatusCode remidy::PluginInstanceAUv3::startProcessing() {
    host_transport_info.currentSample = 0.0;
    host_transport_info.currentBeat = 0.0;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv3::stopProcessing() {
    host_transport_info.isPlaying = false;
    host_transport_info.transportStateChanged = true;
    return StatusCode::OK;
}

remidy::StatusCode remidy::PluginInstanceAUv3::process(AudioProcessContext &process) {
    @autoreleasepool {
        if (audioUnit == nil) {
            logger()->logError("%s: PluginInstanceAUv3::process - audioUnit is nil", name.c_str());
            return StatusCode::FAILED_TO_PROCESS;
        }

        // Update transport info from MasterContext
        auto* trackContext = process.trackContext();
        auto& masterContext = trackContext->masterContext();

        host_transport_info.isPlaying = masterContext.isPlaying();
        host_transport_info.transportStateChanged = false;
        host_transport_info.sampleRate = masterContext.sampleRate();
        // NOTE: The following fields are available but not yet populated from MasterContext:
        // - timeSigNumerator, timeSigDenominator (currently using defaults: 4/4)
        // - isRecording, isCycling, cycleStart, cycleEnd
        // TODO: Add time signature, recording, and cycling support to MasterContext when needed

        // tempo in masterContext is in microseconds per quarter note, convert to BPM
        double tempoBPM = 60000000.0 / masterContext.tempo();
        host_transport_info.currentTempo = tempoBPM;

        if (host_transport_info.currentTempo > 0.0 && host_transport_info.sampleRate > 0.0) {
            double seconds = host_transport_info.currentSample / host_transport_info.sampleRate;
            host_transport_info.currentBeat = seconds * (host_transport_info.currentTempo / 60.0);
        } else {
            host_transport_info.currentBeat = 0.0;
        }

        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AURenderBlock renderBlock = [audioUnit renderBlock];
        if (renderBlock == nil) {
            logger()->logError("%s: PluginInstanceAUv3::process - renderBlock is nil", name.c_str());
            return StatusCode::FAILED_TO_PROCESS;
        }

        // Get output bus format
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AUAudioUnitBusArray* outputBusArray = [audioUnit outputBusses];
        if ([outputBusArray count] == 0) {
            logger()->logError("%s: PluginInstanceAUv3::process - no output busses", name.c_str());
            return StatusCode::FAILED_TO_PROCESS;
        }

        AUAudioUnitBus* mainOutputBus = [outputBusArray objectAtIndexedSubscript:0];
        AVAudioFormat* outputFormat = [mainOutputBus format];
        bool useDouble = [outputFormat commonFormat] == AVAudioPCMFormatFloat64;

        // Prepare output buffer list. AudioBufferList embeds a flexible array member,
        // so we need to allocate enough space for all channels rather than stack-allocating one.
        uint32_t numOutputChannels = process.outputChannelCount(0);
        size_t outputBufferListSize = sizeof(AudioBufferList) + (numOutputChannels > 0 ? (numOutputChannels - 1) * sizeof(AudioBuffer) : 0);
        std::vector<uint8_t> outputBufferStorage(outputBufferListSize);
        AudioBufferList* outputBufferList = reinterpret_cast<AudioBufferList*>(outputBufferStorage.data());
        outputBufferList->mNumberBuffers = numOutputChannels;

        UInt32 sampleSize = useDouble ? sizeof(double) : sizeof(float);
        UInt32 bufferByteSize = process.frameCount() * sampleSize;

        for (uint32_t ch = 0; ch < numOutputChannels; ch++) {
            outputBufferList->mBuffers[ch].mNumberChannels = 1;
            outputBufferList->mBuffers[ch].mDataByteSize = bufferByteSize;
            outputBufferList->mBuffers[ch].mData = useDouble ?
                (void*)process.getDoubleOutBuffer(0, ch) :
                (void*)process.getFloatOutBuffer(0, ch);
        }

        // Some AUv2-based effects expect in-place processing where input audio is already
        // present in the output buffers. Seed the buffers with the host input so both
        // in-place and pull-based processing paths receive audio.
        if (!audio_buses->audioInputBuses().empty() && process.audioInBusCount() > 0) {
            uint32_t inChannels = process.inputChannelCount(0);
            uint32_t channelsToCopy = std::min(inChannels, numOutputChannels);
            for (uint32_t ch = 0; ch < channelsToCopy; ++ch) {
                if (useDouble) {
                    double* dst = process.getDoubleOutBuffer(0, ch);
                    double* src = process.getDoubleInBuffer(0, ch);
                    if (dst && src)
                        std::memcpy(dst, src, bufferByteSize);
                } else {
                    float* dst = process.getFloatOutBuffer(0, ch);
                    float* src = process.getFloatInBuffer(0, ch);
                    if (dst && src)
                        std::memcpy(dst, src, bufferByteSize);
                }
            }
        }

        // Prepare timestamp
        AudioTimeStamp timestamp{};
        timestamp.mSampleTime = host_transport_info.currentSample;
        timestamp.mHostTime = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        timestamp.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;

        AUEventSampleTime eventSampleTime = static_cast<AUEventSampleTime>(timestamp.mSampleTime);
        size_t eventBytes = process.eventIn().position();
        bool midiEventsScheduled = false;
        bool hasMidiBlock = audioUnit.scheduleMIDIEventBlock != nil;
        bool hasMidiListBlock = false;
        if (@available(macOS 12.0, *)) {
            hasMidiListBlock = audioUnit.scheduleMIDIEventListBlock != nil;
        }

        if (eventBytes > 0) {
            if (@available(macOS 12.0, *)) {
                AUMIDIEventListBlock midiListBlock = audioUnit.scheduleMIDIEventListBlock;
                if (midiListBlock && midi_event_list && midi_event_list_capacity >= sizeof(MIDIEventList)) {
                    MIDIEventPacket* packet = MIDIEventListInit(midi_event_list, kMIDIProtocol_1_0);
                    bool buildSucceeded = packet != nullptr;

                    if (buildSucceeded) {
                        const uint8_t* umpData = static_cast<const uint8_t*>(process.eventIn().getMessages());
                        bool unsupportedMessage = false;
                        CMIDI2_UMP_SEQUENCE_FOREACH(umpData, eventBytes, iter) {
                            auto* words = reinterpret_cast<const UInt32*>(iter);
                            auto* ump = reinterpret_cast<cmidi2_ump*>(const_cast<uint8_t*>(iter));
                            if (cmidi2_ump_get_message_type(ump) != CMIDI2_MESSAGE_TYPE_MIDI_1_CHANNEL) {
                                unsupportedMessage = true;
                                break;
                            }
                            ByteCount wordCount = static_cast<ByteCount>(cmidi2_ump_get_message_size_bytes(ump) / sizeof(uint32_t));
                            packet = MIDIEventListAdd(midi_event_list,
                                                      midi_event_list_capacity,
                                                      packet,
                                                      0,
                                                      wordCount,
                                                      words);
                            if (packet == nullptr) {
                                buildSucceeded = false;
                                logger()->logError("%s: MIDIEventListAdd failed - buffer too small", name.c_str());
                                break;
                            }
                        }
                        if (unsupportedMessage) {
                            buildSucceeded = false;
                        }
                    }

                    if (buildSucceeded) {
                        OSStatus midiStatus = midiListBlock(eventSampleTime, 0, midi_event_list);
                        if (midiStatus != noErr) {
                            logger()->logError("%s: scheduleMIDIEventListBlock failed with status %d",
                                               name.c_str(),
                                               midiStatus);
                        } else {
                            midiEventsScheduled = true;
                        }
                    }
                } else if (!midiEventsScheduled && midi_event_list && midi_event_list_capacity < sizeof(MIDIEventList)) {
                    logger()->logWarning("%s: MIDI event list buffer too small (%zu bytes)",
                                         name.c_str(),
                                         midi_event_list_capacity);
                }
            }

            if (!midiEventsScheduled && midiConverter && audioUnit.scheduleMIDIEventBlock) {
                AURenderEvent* midiEvents = midiConverter->convertUMPToRenderEvents(process.eventIn(), eventSampleTime);

                if (midiEvents) {
                    AURenderEvent* evt = midiEvents;
                    while (evt != nullptr) {
                        if (evt->head.eventType == AURenderEventMIDI) {
                            audioUnit.scheduleMIDIEventBlock(evt->head.eventSampleTime,
                                                             evt->MIDI.cable,
                                                             evt->MIDI.length,
                                                             evt->MIDI.data);
                        }
                        evt = evt->head.next;
                    }
                    midiEventsScheduled = true;
                }
            }

            if (!midiEventsScheduled) {
                logger()->logWarning("%s: Dropped %zu MIDI bytes (listBlock=%s, midiBlock=%s)",
                                     name.c_str(),
                                     eventBytes,
                                     hasMidiListBlock ? "yes" : "no",
                                     hasMidiBlock ? "yes" : "no");
            }
        }

        // Call the render block
        // NOTE: some non-trivial replacement during AUv2->AUv3 migration
        AudioUnitRenderActionFlags actionFlags = 0;

        // Create input pull block only when needed
        const bool hasInputBuses = process.audioInBusCount() > 0 && !audio_buses->audioInputBuses().empty();
        AURenderPullInputBlock inputBlock = nil;
        if (hasInputBuses) {
            inputBlock = ^AUAudioUnitStatus(
                AudioUnitRenderActionFlags *pullActionFlags,
                const AudioTimeStamp *pullTimestamp,
                AUAudioFrameCount pullFrameCount,
                NSInteger inputBusNumber,
                AudioBufferList *inputData) {

                if (inputBusNumber < 0 ||
                    inputBusNumber >= process.audioInBusCount() ||
                    inputBusNumber >= static_cast<NSInteger>(audio_buses->audioInputBuses().size())) {
                    // Bus not connected; provide silence but do not treat as fatal.
                    if (inputData) {
                        inputData->mNumberBuffers = 0;
                        if (pullActionFlags)
                            *pullActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
                    }
                    return noErr;
                }

                uint32_t numInputChannels = process.inputChannelCount(inputBusNumber);
                inputData->mNumberBuffers = numInputChannels;

                UInt32 pullBufferSize = pullFrameCount * (useDouble ? sizeof(double) : sizeof(float));

                for (uint32_t ch = 0; ch < numInputChannels; ch++) {
                    inputData->mBuffers[ch].mNumberChannels = 1;
                    inputData->mBuffers[ch].mDataByteSize = pullBufferSize;
                    inputData->mBuffers[ch].mData = useDouble ?
                        (void*)process.getDoubleInBuffer(inputBusNumber, ch) :
                        (void*)process.getFloatInBuffer(inputBusNumber, ch);
                    if (!inputData->mBuffers[ch].mData) {
                        inputData->mBuffers[ch].mDataByteSize = 0;
                    }
                }

                return noErr;
            };
        }

        AUAudioUnitStatus status = renderBlock(&actionFlags,
                                               &timestamp,
                                               process.frameCount(),
                                               0, // output bus number
                                               outputBufferList,
                                               inputBlock);

        if (status != noErr) {
            logger()->logError("%s: PluginInstanceAUv3::process - renderBlock failed with status %d",
                             name.c_str(), status);
            return StatusCode::FAILED_TO_PROCESS;
        }

        // Update sample position for next call
        host_transport_info.currentSample += process.frameCount();

        return StatusCode::OK;
    }
}

#endif
