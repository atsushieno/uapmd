#ifdef __EMSCRIPTEN__

#include "WebAudioWorkletIODevice.hpp"
#include <emscripten/webaudio.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace uapmd {

    // ── WebAudioEngineThread ──────────────────────────────────────────────────

    WebAudioEngineThread::WebAudioEngineThread(SequencerEngine* engine,
                                               WebAudioSAB* sab,
                                               uint32_t sampleRate,
                                               uint32_t bufferSize)
        : engine_(engine)
        , sab_(sab)
        , sample_rate_(sampleRate)
        , buffer_size_(bufferSize)
        , engine_ctx_(engine_master_ctx_, bufferSize * 4)
        , pump_ctx_(pump_master_ctx_, bufferSize * 4)
    {
        engine_ctx_.configureMainBus(0, kWebAudioChannels, bufferSize);
        engine_ctx_.frameCount(static_cast<int32_t>(bufferSize));
        pump_ctx_.configureMainBus(0, kWebAudioChannels, bufferSize);
        pump_ctx_.frameCount(static_cast<int32_t>(bufferSize));
    }

    WebAudioEngineThread::~WebAudioEngineThread() {
        stop();
    }

    void WebAudioEngineThread::start() {
        if (running_.load(std::memory_order_relaxed))
            return;
        running_.store(true, std::memory_order_release);
        engine_->setExternalPump(true);
        pump_thread_   = std::thread([this]{ pumpLoop(); });
        engine_thread_ = std::thread([this]{ engineLoop(); });
    }

    void WebAudioEngineThread::stop() {
        if (!running_.load(std::memory_order_relaxed))
            return;
        running_.store(false, std::memory_order_release);
        if (engine_thread_.joinable()) engine_thread_.join();
        if (pump_thread_.joinable())   pump_thread_.join();
        engine_->setExternalPump(false);
    }

    void WebAudioEngineThread::engineLoop() {
        uint64_t last_seq = 0;
        while (running_.load(std::memory_order_acquire)) {
            uint64_t seq = sab_->host_seq.load(std::memory_order_acquire);
            if (seq == last_seq) {
                std::this_thread::yield();
                continue;
            }
            last_seq = seq;

            try {
                engine_->processAudio(engine_ctx_);
            } catch (const std::exception& e) {
                std::cerr << "[WebAudio] engineLoop processAudio exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[WebAudio] engineLoop processAudio unknown exception" << std::endl;
            }

            // Copy buffer_size_ frames of planar output to SAB master_output.
            // Layout: ch0[0..buffer_size_) then ch1[0..buffer_size_)
            for (uint32_t ch = 0; ch < kWebAudioChannels; ch++) {
                const float* src = engine_ctx_.getFloatOutBuffer(0, ch);
                float* dst = sab_->master_output + ch * buffer_size_;
                if (src)
                    std::memcpy(dst, src, buffer_size_ * sizeof(float));
                else
                    std::memset(dst, 0, buffer_size_ * sizeof(float));
            }

            sab_->engine_seq.store(seq, std::memory_order_release);
        }
    }

    void WebAudioEngineThread::pumpLoop() {
        using clock = std::chrono::steady_clock;
        // One quantum (buffer_size_ frames) of wall-clock time between pump calls.
        const auto pump_interval = std::chrono::nanoseconds(
            static_cast<int64_t>(1'000'000'000LL * buffer_size_ / sample_rate_));
        auto next_time = clock::now();
        while (running_.load(std::memory_order_acquire)) {
            const auto now = clock::now();
            if (now < next_time) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            next_time += pump_interval;
            // If we fell far behind (scheduler hiccup), clamp to avoid spiral.
            if (next_time < now)
                next_time = now + pump_interval;
            try {
                engine_->pumpAudio(pump_ctx_);
            } catch (const std::exception& e) {
                std::cerr << "[WebAudio] pumpLoop pumpAudio exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[WebAudio] pumpLoop pumpAudio unknown exception" << std::endl;
            }
        }
    }

    // ── WebAudioWorkletIODevice ───────────────────────────────────────────────

    WebAudioWorkletIODevice::WebAudioWorkletIODevice(uint32_t sampleRate, uint32_t bufferSize)
        : sample_rate_(sampleRate)
        , buffer_size_(bufferSize)
    {
    }

    WebAudioWorkletIODevice::~WebAudioWorkletIODevice() {
        stop();
        if (worklet_node_)  emscripten_destroy_web_audio_node(worklet_node_);
        if (audio_ctx_)     emscripten_destroy_audio_context(audio_ctx_);
    }

    void WebAudioWorkletIODevice::setEngine(SequencerEngine* engine) {
        if (!engine) {
            engine_thread_ready_.store(nullptr, std::memory_order_release);
            engine_thread_.reset();
            return;
        }
        engine_thread_ = std::make_unique<WebAudioEngineThread>(
            engine, &sab_, sample_rate_, buffer_size_);
        if (is_playing_) {
            engine_thread_->start();
            engine_thread_ready_.store(engine_thread_.get(), std::memory_order_release);
        }
    }

    // ── Async worklet startup chain ───────────────────────────────────────────

    struct WorkletStartCtx {
        WebAudioWorkletIODevice* device;
    };

    static void onWorkletProcessorCreated(EMSCRIPTEN_WEBAUDIO_T audioCtx, bool success, void* userData) {
        auto* ctx = static_cast<WorkletStartCtx*>(userData);
        if (!success) { delete ctx; return; }

        int outputChannelCounts[] = { static_cast<int>(kWebAudioChannels) };
        EmscriptenAudioWorkletNodeCreateOptions opts{
            .numberOfInputs  = 0,
            .numberOfOutputs = 1,
            .outputChannelCounts = outputChannelCounts,
        };
        auto node = emscripten_create_wasm_audio_worklet_node(
            audioCtx, "uapmd-processor", &opts,
            &WebAudioWorkletIODevice::audioProcessCallback, ctx->device);
        ctx->device->onWorkletNodeCreated(node);
        emscripten_audio_node_connect(node, audioCtx, 0, 0);
        delete ctx;
    }

    static void onWorkletThreadStarted(EMSCRIPTEN_WEBAUDIO_T audioCtx, bool success, void* userData) {
        auto* ctx = static_cast<WorkletStartCtx*>(userData);
        if (!success) { delete ctx; return; }

        WebAudioWorkletProcessorCreateOptions procOpts{
            .name = "uapmd-processor",
            .numAudioParams = 0,
            .audioParamDescriptors = nullptr,
        };
        emscripten_create_wasm_audio_worklet_processor_async(
            audioCtx, &procOpts, onWorkletProcessorCreated, ctx);
    }

    static uint8_t sAudioWorkletStack[8192] __attribute__((aligned(16)));

    uapmd_status_t WebAudioWorkletIODevice::start() {
        if (is_playing_)
            return 0;
        is_playing_ = true;

        if (engine_thread_) {
            engine_thread_->start();
            // Publish after start() so the AudioWorklet thread sees this only
            // once the pthreads are running and setExternalPump(true) is done.
            engine_thread_ready_.store(engine_thread_.get(), std::memory_order_release);
        }

        EmscriptenWebAudioCreateAttributes attrs{
            .latencyHint = "interactive",
            .sampleRate  = sample_rate_,
        };
        audio_ctx_ = emscripten_create_audio_context(&attrs);
        emscripten_resume_audio_context_sync(audio_ctx_);

        auto* ctx = new WorkletStartCtx{ this };
        emscripten_start_wasm_audio_worklet_thread_async(
            audio_ctx_, sAudioWorkletStack, sizeof(sAudioWorkletStack),
            onWorkletThreadStarted, ctx);
        return 0;
    }

    uapmd_status_t WebAudioWorkletIODevice::stop() {
        if (!is_playing_)
            return 0;
        is_playing_ = false;
        engine_thread_ready_.store(nullptr, std::memory_order_release);
        if (engine_thread_)
            engine_thread_->stop();
        return 0;
    }

    // ── AudioWorklet process callback (runs on AudioWorklet WASM Worker) ──────

    bool WebAudioWorkletIODevice::audioProcessCallback(
        int /*numInputs*/, const AudioSampleFrame* /*inputs*/,
        int numOutputs, AudioSampleFrame* outputs,
        int /*numParams*/, const AudioParamFrame* /*params*/,
        void* userData)
    {
        auto* self = static_cast<WebAudioWorkletIODevice*>(userData);

        // Use the atomic flag — guarantees visibility across the main-thread write
        // in start() and this AudioWorklet-thread read.
        auto* engineThread = self->engine_thread_ready_.load(std::memory_order_acquire);

        if (engineThread) {
            const uint32_t engine_quantum = engineThread->bufferSize();
            const uint32_t quanta_per_render = engine_quantum / kWebAudioQuantum;

            // On the first slice of a new engine render, trigger processAudio().
            if (self->accum_count_ == 0) {
                uint64_t seq = self->sab_.host_seq.load(std::memory_order_relaxed) + 1;
                self->sab_.host_seq.store(seq, std::memory_order_release);
                // Spin-wait for engine to finish its full engine_quantum render.
                while (self->sab_.engine_seq.load(std::memory_order_acquire) != seq)
                    ; // engine must finish before the 128-frame AudioWorklet deadline
            }

            // Serve slice accum_count_ (128 frames) from the SAB.
            if (numOutputs > 0) {
                const int samplesPerCh = outputs[0].samplesPerChannel;
                const uint32_t chCount = std::min(
                    static_cast<uint32_t>(outputs[0].numberOfChannels), kWebAudioChannels);
                const uint32_t offset = self->accum_count_ * kWebAudioQuantum;
                for (uint32_t ch = 0; ch < chCount; ch++) {
                    // Planar layout: ch0[0..engine_quantum) | ch1[0..engine_quantum) | ...
                    const float* src = self->sab_.master_output + ch * engine_quantum + offset;
                    float* dst = outputs[0].data + ch * samplesPerCh;
                    std::memcpy(dst, src, kWebAudioQuantum * sizeof(float));
                }
            }

            if (++self->accum_count_ >= quanta_per_render)
                self->accum_count_ = 0;
        } else {
            // Engine thread not ready yet: output silence.
            // Never call processAudio/pumpAudio from the AudioWorklet thread —
            // they access mutexes (std::mutex → Atomics.wait) which are
            // forbidden on the AudioWorklet rendering thread.
            if (numOutputs > 0) {
                const int samplesPerCh = outputs[0].samplesPerChannel;
                const uint32_t chCount = static_cast<uint32_t>(outputs[0].numberOfChannels);
                for (uint32_t ch = 0; ch < chCount; ch++)
                    std::memset(outputs[0].data + ch * samplesPerCh, 0,
                                samplesPerCh * sizeof(float));
            }
        }
        return true; // keep processing
    }

    // ── WebAudioWorkletIODeviceManager ────────────────────────────────────────

    WebAudioWorkletIODeviceManager::WebAudioWorkletIODeviceManager()
        : AudioIODeviceManager("webaudio") {}

    void WebAudioWorkletIODeviceManager::initialize(Configuration& config) {
        (void) config;
        initialized = true;
    }

    std::vector<AudioIODeviceInfo> WebAudioWorkletIODeviceManager::onDevices() {
        return {{ .directions = UAPMD_AUDIO_DIRECTION_OUTPUT,
                  .id = 0, .name = "WebAudio", .sampleRate = 48000, .channels = 2 }};
    }

    std::vector<uint32_t> WebAudioWorkletIODeviceManager::getDeviceSampleRates(
        const std::string&, AudioIODirections)
    {
        return { 44100, 48000 };
    }

    std::vector<int> WebAudioWorkletIODeviceManager::getAvailableBufferSizes() const {
        std::vector<int> sizes;
        for (uint32_t n = kWebAudioQuantum; n <= kWebAudioMaxQuantum; n += kWebAudioQuantum)
            sizes.push_back(static_cast<int>(n));
        return sizes;
    }

    AudioIODevice* WebAudioWorkletIODeviceManager::onOpen(
        int /*inputDeviceIndex*/, int /*outputDeviceIndex*/,
        uint32_t sampleRate, uint32_t bufferSize)
    {
        if (sampleRate == 0) sampleRate = 48000;
        // The engine quantum must be a multiple of kWebAudioQuantum (128) and no
        // larger than kWebAudioMaxQuantum.  The AudioWorklet fires every 128 frames
        // regardless; for N > 1 the callback accumulates N slices before triggering
        // the next engine render.
        if (bufferSize < kWebAudioQuantum)
            bufferSize = kWebAudioQuantum;
        bufferSize = (bufferSize / kWebAudioQuantum) * kWebAudioQuantum;
        if (bufferSize > kWebAudioMaxQuantum)
            bufferSize = kWebAudioMaxQuantum;
        return new WebAudioWorkletIODevice(sampleRate, bufferSize);
    }

} // namespace uapmd

#endif // __EMSCRIPTEN__
