#include "uapmd/uapmd.hpp"
#include "WebAudioWorkletIODevice.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/webaudio.h>
#endif

#include <algorithm>
#include <cstring>
#include <iostream>

namespace uapmd {

// ── WebAudioWorkletIODeviceManager ───────────────────────────────────────────

WebAudioWorkletIODeviceManager::WebAudioWorkletIODeviceManager()
    : AudioIODeviceManager("webaudioworklet") {}

void WebAudioWorkletIODeviceManager::initialize(Configuration& /*config*/) {
    initialized = true;
}

std::vector<AudioIODeviceInfo> WebAudioWorkletIODeviceManager::onDevices() {
    return { AudioIODeviceInfo{
        .directions = UAPMD_AUDIO_DIRECTION_OUTPUT,
        .id         = 0,
        .name       = "Web AudioWorklet",
        .sampleRate = 48000,
        .channels   = WebAudioWorkletIODevice::kChannels,
    }};
}

AudioIODevice* WebAudioWorkletIODeviceManager::onOpen(
    int /*inputDeviceIndex*/, int /*outputDeviceIndex*/,
    uint32_t sampleRate, uint32_t /*bufferSize*/)
{
    static WebAudioWorkletIODevice device{sampleRate > 0 ? sampleRate : 48000u};
    return &device;
}

std::vector<uint32_t> WebAudioWorkletIODeviceManager::getDeviceSampleRates(
    const std::string& /*deviceName*/, AudioIODirections /*direction*/)
{
    return {48000u};
}

// ── WebAudioWorkletIODevice ───────────────────────────────────────────────────

WebAudioWorkletIODevice::WebAudioWorkletIODevice(uint32_t sampleRate)
    : sample_rate_(sampleRate > 0 ? sampleRate : 48000u),
      audio_ctx_(master_ctx_, 0 /* no UMP events at device level */)
{
    // Initialise the MasterContext sample rate so downstream engine code sees
    // the correct value before the first processAudio call.
    master_ctx_.sampleRate(static_cast<int32_t>(sample_rate_));

    // Pre-configure for stereo output with the fixed 128-frame AudioWorklet
    // quantum.  The engine may reconfigure channels later via
    // DefaultDeviceIODispatcher / RealtimeSequencer.
    audio_ctx_.configureMainBus(0, static_cast<int32_t>(kChannels),
                                static_cast<size_t>(kQuantum));
    audio_ctx_.frameCount(static_cast<int32_t>(kQuantum));
}

WebAudioWorkletIODevice::~WebAudioWorkletIODevice() {
    stop();
}

// ── AudioWorklet callbacks (Emscripten-only) ──────────────────────────────────

#ifdef __EMSCRIPTEN__

// Called by the AudioWorklet WASM Worker on every 128-frame quantum.
// Must be non-blocking and real-time safe.
EM_BOOL WebAudioWorkletIODevice::audioProcessCallback(
    int /*numInputs*/,  const AudioSampleFrame* /*inputs*/,
    int  numOutputs,          AudioSampleFrame*  outputs,
    int /*numParams*/, const AudioParamFrame*  /*params*/,
    void* userData)
{
    auto* self = static_cast<WebAudioWorkletIODevice*>(userData);
    if (!self || numOutputs < 1)
        return EM_TRUE;

    const int32_t nFrames = outputs[0].samplesPerChannel; // always 128
    self->audio_ctx_.frameCount(nFrames);
    self->audio_ctx_.clearAudioOutputs();

    // Drive the engine (and all queued transport-command flushes via
    // ThreadedEngineProxy) for this 128-frame block.
    for (auto& cb : self->callbacks_)
        cb(self->audio_ctx_);

    // Copy deinterleaved output: AudioProcessContext channel buffers
    // → AudioSampleFrame::data   (layout: [ch0_f0..f127, ch1_f0..f127]).
    const int nOut = std::min(outputs[0].numberOfChannels,
                              static_cast<int>(kChannels));
    for (int ch = 0; ch < nOut; ++ch) {
        const float* src = self->audio_ctx_.getFloatOutBuffer(
            0, static_cast<uint32_t>(ch));
        float* dst = outputs[0].data + ch * nFrames;
        if (src)
            std::memcpy(dst, src, static_cast<size_t>(nFrames) * sizeof(float));
        else
            std::memset(dst, 0,   static_cast<size_t>(nFrames) * sizeof(float));
    }

    return EM_TRUE; // keep the worklet alive
}

// Called on the main thread once the AudioWorklet WASM Worker is ready to
// accept AudioWorkletNodes.
void WebAudioWorkletIODevice::onWorkletThreadReady(
    EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void* userData)
{
    auto* self = static_cast<WebAudioWorkletIODevice*>(userData);
    if (!success) {
        std::cerr << "[WebAudioWorklet] AudioWorklet thread failed to start\n";
        self->starting_.store(false, std::memory_order_release);
        return;
    }

    WebAudioWorkletProcessorCreateOptions processorOptions{};
    processorOptions.name                = "uapmd-processor";
    processorOptions.numAudioParams      = 0;
    processorOptions.audioParamDescriptors = nullptr;

    emscripten_create_wasm_audio_worklet_processor_async(
        ctx, &processorOptions, &onProcessorRegistered, self);
}

void WebAudioWorkletIODevice::onProcessorRegistered(
    EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void* userData)
{
    auto* self = static_cast<WebAudioWorkletIODevice*>(userData);
    if (!success) {
        std::cerr << "[WebAudioWorklet] Failed to register processor 'uapmd-processor'\n";
        self->starting_.store(false, std::memory_order_release);
        return;
    }

    int outputChannelCounts[] = { static_cast<int>(kChannels) };
    EmscriptenAudioWorkletNodeCreateOptions options{
        .numberOfInputs      = 0,
        .numberOfOutputs     = 1,
        .outputChannelCounts = outputChannelCounts,
    };

    self->worklet_node_ = emscripten_create_wasm_audio_worklet_node(
        ctx, "uapmd-processor", &options, &audioProcessCallback, self);
    if (!self->worklet_node_) {
        std::cerr << "[WebAudioWorklet] Failed to create AudioWorklet node\n";
        self->starting_.store(false, std::memory_order_release);
        return;
    }

    emscripten_audio_node_connect(self->worklet_node_, ctx, 0, 0);

    self->starting_.store(false, std::memory_order_release);
    self->playing_.store(true, std::memory_order_release);
    std::cout << "[WebAudioWorklet] AudioWorklet started at "
              << self->sample_rate_ << " Hz\n";
}

uapmd_status_t WebAudioWorkletIODevice::start()
{
    if (playing_.load(std::memory_order_acquire))
        return 0;

    // If the AudioContext and worklet node already exist from a previous
    // start/stop cycle, don't create a new one — just re-enable playing.
    // (AudioContexts are not restartable; creating a new one every time
    // stop()+start() is called leaks the old one and runs two worklets.)
    if (audio_context_ && worklet_node_) {
        playing_.store(true, std::memory_order_release);
        return 0;
    }

    // Prevent a second concurrent start() call that races before playing_ is
    // set to true inside onWorkletThreadReady (which fires asynchronously).
    bool expected = false;
    if (!starting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return 0;

    // Create a Web AudioContext.  The browser will suspend it until the
    // first user-gesture; modern browsers auto-resume on interaction.
    EmscriptenWebAudioCreateAttributes attrs{};
    attrs.latencyHint = "interactive";
    attrs.sampleRate  = static_cast<int>(sample_rate_);

    audio_context_ = emscripten_create_audio_context(&attrs);
    if (!audio_context_) {
        std::cerr << "[WebAudioWorklet] emscripten_create_audio_context failed\n";
        starting_.store(false, std::memory_order_release);
        return -1;
    }

    // Fire the audio callbacks once on the main thread before the AudioWorklet
    // worker spins up.  Any lazy-initialized statics (std::locale caches, logger
    // singletons, etc.) will finish construction outside the audio thread, which
    // avoids Atomics.wait usage inside the worklet (prohibited by Chrome).
    if (!callbacks_.empty() && !warmed_up_.exchange(true, std::memory_order_acq_rel)) {
        audio_ctx_.frameCount(static_cast<int32_t>(kQuantum));
        audio_ctx_.clearAudioOutputs();
        for (auto& cb : callbacks_)
            cb(audio_ctx_);
    }

    // Kick off the AudioWorklet WASM Worker; onWorkletThreadReady fires when
    // the worker is ready to accept AudioWorkletNode registrations.
    emscripten_start_wasm_audio_worklet_thread_async(
        audio_context_,
        worklet_stack_,
        kStackSize,
        onWorkletThreadReady,
        this);

    return 0;
}

uapmd_status_t WebAudioWorkletIODevice::stop()
{
    playing_.store(false, std::memory_order_release);
    // The AudioWorklet Worker keeps running until the AudioContext is closed
    // or the page unloads; calling stop() just tells the rest of the engine
    // that we're not "playing".  A full teardown would require
    // emscripten_audio_context_close() (not yet exposed in public API).
    return 0;
}

#else // !__EMSCRIPTEN__

// Stub implementations so this translation unit compiles on non-WASM targets
// (which should never instantiate WebAudioWorkletIODevice).
uapmd_status_t WebAudioWorkletIODevice::start() { return -1; }
uapmd_status_t WebAudioWorkletIODevice::stop()  { return  0; }

#endif // __EMSCRIPTEN__

// ── Global factory ────────────────────────────────────────────────────────────

static std::unique_ptr<WebAudioWorkletIODeviceManager> web_manager{};

} // namespace uapmd
