#pragma once

#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/webaudio.h>
#endif

namespace uapmd {

// ── Manager ───────────────────────────────────────────────────────────────────

class WebAudioWorkletIODeviceManager final : public AudioIODeviceManager {
public:
    WebAudioWorkletIODeviceManager();

    void initialize(Configuration& config) override;
    std::vector<uint32_t> getDeviceSampleRates(const std::string& deviceName,
                                                AudioIODirections  direction) override;
    bool platformProvidesAutoBufferSize() const override { return false; }

protected:
    std::vector<AudioIODeviceInfo> onDevices() override;
    AudioIODevice* onOpen(int inputDeviceIndex, int outputDeviceIndex,
                          uint32_t sampleRate, uint32_t bufferSize) override;
};

// ── Device ────────────────────────────────────────────────────────────────────
//
// AudioIODevice implementation backed by the Emscripten AudioWorklet API.
//
// Required link flags: -sAUDIO_WORKLET=1 -sWASM_WORKERS=1 -sSHARED_MEMORY=1
//
// Threading model:
//   - All setup (configure, addAudioCallback) runs on the main browser thread.
//   - After start(), the audio callback fires on the AudioWorklet WASM Worker.
//   - Both threads share the same WASM linear memory (SharedArrayBuffer), so
//     the audio_ctx_ and master_ctx_ members are directly visible to both.
//   - ASYNCIFY-suspended functions must only be entered from the main thread;
//     the AudioWorklet callback must never trigger ASYNCIFY unwinding.
//
// Buffer format:
//   AudioWorklet delivers exactly 128 frames per quantum (Web Audio spec).
//   AudioSampleFrame::data is deinterleaved: [ch0_f0..ch0_f127, ch1_f0..].
//   This matches AudioProcessContext::getFloatOutBuffer(bus, ch), so the
//   copy is a simple memcpy per channel.

class WebAudioWorkletIODevice final : public AudioIODevice {
public:
    // AudioWorklet always delivers exactly 128 frames (Web Audio spec).
    static constexpr uint32_t kQuantum   = 128;
    static constexpr uint32_t kChannels  = 2;

    explicit WebAudioWorkletIODevice(uint32_t sampleRate);
    ~WebAudioWorkletIODevice() override;

    void addAudioCallback(
        std::function<uapmd_status_t(AudioProcessContext& data)>&& callback) override {
        callbacks_.emplace_back(std::move(callback));
    }
    void clearAudioCallbacks() override { callbacks_.clear(); }
    void clearOutputBuffers()  override { audio_ctx_.clearAudioOutputs(); }

    // Preferred callback size is fixed by the Web Audio spec.
    void     setPreferredCallbackSize(uint32_t) override {}
    uint32_t preferredCallbackSize()  const override { return kQuantum; }

    double   sampleRate()       override { return static_cast<double>(sample_rate_); }
    uint32_t channels()         override { return kChannels; }
    uint32_t inputChannels()    override { return 0; }   // audio input not supported on WASM
    uint32_t outputChannels()   override { return kChannels; }
    std::vector<uint32_t> getNativeSampleRates() override { return {sample_rate_}; }

    uapmd_status_t start() override;
    uapmd_status_t stop()  override;
    bool isPlaying() override { return playing_.load(std::memory_order_acquire); }

    bool useAutoBufferSize()      override { return false; }
    bool useAutoBufferSize(bool)  override { return false; }

#ifdef __EMSCRIPTEN__
    // Called from the AudioWorklet WASM Worker on every 128-frame quantum.
    // Declared static so Emscripten can call it as a plain function pointer.
    static EM_BOOL audioProcessCallback(
        int numInputs,  const AudioSampleFrame* inputs,
        int numOutputs,       AudioSampleFrame* outputs,
        int numParams,  const AudioParamFrame*  params,
        void* userData);

    // Called on the main thread once the AudioWorklet WASM Worker is ready.
    static void onWorkletThreadReady(
        EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void* userData);

    // Called after the processor registration completes.
    static void onProcessorRegistered(
        EMSCRIPTEN_WEBAUDIO_T ctx, EM_BOOL success, void* userData);
#endif // __EMSCRIPTEN__

private:
    // Stack for the AudioWorklet WASM Worker.  Must outlive the worker.
    // 4 KB is far too small: Emscripten's own thread-init code plus one level
    // of our engine callback already exceeds it, causing audioWorkletCreationFailed.
    // 64 KB gives comfortable headroom for the full plugin-graph traversal.
    static constexpr int kStackSize = 65536;

    uint32_t sample_rate_;

#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBAUDIO_T           audio_context_{0};
    EMSCRIPTEN_AUDIO_WORKLET_NODE_T worklet_node_{0};
    alignas(16) uint8_t worklet_stack_[kStackSize]{};
#endif

    std::vector<std::function<uapmd_status_t(AudioProcessContext& data)>> callbacks_;
    std::atomic<bool> playing_{false};
    std::atomic<bool> starting_{false};
    std::atomic<bool> warmed_up_{false};

    // Audio processing state shared with the AudioWorklet WASM Worker.
    // These objects live in WASM linear memory (SharedArrayBuffer when
    // SHARED_MEMORY=1), so the Worker can access them directly.
    remidy::MasterContext       master_ctx_;
    remidy::AudioProcessContext audio_ctx_;
};

} // namespace uapmd
