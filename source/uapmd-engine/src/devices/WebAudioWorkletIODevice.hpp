#pragma once

#ifdef __EMSCRIPTEN__

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <emscripten/webaudio.h>

#include "uapmd/uapmd.hpp"
#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd {

    // ── Shared Audio Buffer (SAB) ─────────────────────────────────────────────
    //
    // Layout shared between the AudioWorklet WASM Worker thread and the engine
    // pthread.  Requires SharedArrayBuffer (COOP/COEP headers on the server).
    //
    //  host_seq   — incremented by AudioWorklet after copying inputs to audio_input
    //  engine_seq — incremented by engine pthread after copying outputs to master_output
    //
    // Both counters are even when idle and odd while a frame is in flight,
    // but the simpler "host writes N, engine writes N back" approach is used here:
    // host spins until engine_seq == host_seq, then reads master_output.

    static constexpr uint32_t kWebAudioChannels   = 2;
    // The Web Audio API fires the AudioWorklet callback in exactly 128-frame
    // quanta.  This is immutable per the Web Audio spec.
    static constexpr uint32_t kWebAudioQuantum    = 128;
    // The engine buffer size may be any multiple of kWebAudioQuantum up to this
    // limit.  The SAB master_output array is sized to hold one full engine render.
    static constexpr uint32_t kWebAudioMaxQuantum = 2048;
    static constexpr uint32_t kWebAudioBufferSize = kWebAudioChannels * kWebAudioMaxQuantum;

    struct WebAudioSAB {
        std::atomic<uint64_t> host_seq{0};
        std::atomic<uint64_t> engine_seq{0};
        float audio_input [kWebAudioChannels * kWebAudioQuantum]{};  // planar L/R from AudioWorklet (always 128 frames)
        float master_output[kWebAudioBufferSize]{};                  // planar L/R to AudioWorklet (up to kWebAudioMaxQuantum frames)
    };

    // ── WebAudioEngineThread ──────────────────────────────────────────────────
    //
    // Owns two pthreads:
    //   engine_thread_ — spins on host_seq, calls processAudio(), copies output
    //   pump_thread_   — calls pumpAudio() in a loop (drives the ring-buffer pump)
    //
    // Lifecycle: constructed by WebAudioWorkletIODevice::setEngine(), started by
    // start(), stopped by stop().

    class WebAudioEngineThread {
    public:
        explicit WebAudioEngineThread(SequencerEngine* engine,
                                      WebAudioSAB* sab,
                                      uint32_t sampleRate,
                                      uint32_t bufferSize);
        ~WebAudioEngineThread();

        void start();
        void stop();

        uint32_t bufferSize() const { return buffer_size_; }

    private:
        SequencerEngine* engine_;
        WebAudioSAB*      sab_;
        uint32_t         sample_rate_;
        uint32_t         buffer_size_;

        std::atomic<bool> running_{false};
        std::thread       engine_thread_;
        std::thread       pump_thread_;

        MasterContext     engine_master_ctx_;
        MasterContext     pump_master_ctx_;
        AudioProcessContext engine_ctx_;  // used by engine thread
        AudioProcessContext pump_ctx_;    // used by pump thread

        void engineLoop();
        void pumpLoop();
    };

    // ── WebAudioWorkletIODevice / Manager ─────────────────────────────────────

    class WebAudioWorkletIODevice : public AudioIODevice {
    public:
        WebAudioWorkletIODevice(uint32_t sampleRate, uint32_t bufferSize);
        ~WebAudioWorkletIODevice() override;

        // Called by RealtimeSequencer after configure() to inject the engine.
        void setEngine(SequencerEngine* engine) override;

        void addAudioCallback(std::function<uapmd_status_t(AudioProcessContext&)>&& cb) override {
            callbacks_.emplace_back(std::move(cb));
        }
        void clearAudioCallbacks() override { callbacks_.clear(); }

        double   sampleRate()    override { return sample_rate_; }
        uint32_t channels()      override { return kWebAudioChannels; }
        uint32_t outputChannels() override { return kWebAudioChannels; }
        uint32_t inputChannels()  override { return 0; }
        std::vector<uint32_t> getNativeSampleRates() override { return {sample_rate_}; }

        uapmd_status_t start()   override;
        uapmd_status_t stop()    override;
        bool isPlaying()         override { return is_playing_; }
        bool useAutoBufferSize() override { return false; }
        bool useAutoBufferSize(bool) override { return false; }

        // Called from the AudioWorklet WASM Worker thread each quantum.
        // When engine_thread_ is set: rendezvous via SAB.
        // Fallback (engine_thread_ == nullptr): call callbacks_ directly.
        static bool audioProcessCallback(int numInputs, const AudioSampleFrame* inputs,
                                         int numOutputs, AudioSampleFrame* outputs,
                                         int numParams, const AudioParamFrame* params,
                                         void* userData);

        // Called by the worklet startup chain (file-static callbacks in the .cpp)
        // once the AudioWorkletNode is created and connected.
        void onWorkletNodeCreated(EMSCRIPTEN_AUDIO_WORKLET_NODE_T node) { worklet_node_ = node; }

    private:
        uint32_t sample_rate_;
        uint32_t buffer_size_;
        bool     is_playing_{false};

        std::vector<std::function<uapmd_status_t(AudioProcessContext&)>> callbacks_;

        // Counts how many kWebAudioQuantum-sized slices have been served from the
        // current engine render.  Resets to 0 after buffer_size_/kWebAudioQuantum
        // slices.  Accessed only from the AudioWorklet thread — no synchronisation
        // needed.
        uint32_t                               accum_count_{0};

        WebAudioSAB                            sab_{};
        std::unique_ptr<WebAudioEngineThread> engine_thread_{};
        // Atomic flag set after engine_thread_ is fully started; read by the
        // AudioWorklet thread (different thread) to determine which path to take.
        std::atomic<WebAudioEngineThread*>    engine_thread_ready_{nullptr};

        EMSCRIPTEN_WEBAUDIO_T           audio_ctx_{0};
        EMSCRIPTEN_AUDIO_WORKLET_NODE_T worklet_node_{0};

    };

    class WebAudioWorkletIODeviceManager : public AudioIODeviceManager {
    public:
        WebAudioWorkletIODeviceManager();
        void initialize(Configuration& config) override;
        std::vector<AudioIODeviceInfo> onDevices() override;
        std::vector<uint32_t> getDeviceSampleRates(const std::string&, AudioIODirections) override;
        bool platformProvidesAutoBufferSize() const override { return false; }
        // Only multiples of kWebAudioQuantum are valid engine quanta for Web Audio.
        std::vector<int> getAvailableBufferSizes() const override;
    protected:
        AudioIODevice* onOpen(int, int, uint32_t sampleRate, uint32_t bufferSize) override;
    };

} // namespace uapmd

#endif // __EMSCRIPTEN__
