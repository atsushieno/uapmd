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
        engine_->setTrackOutputHandler([this](uapmd_track_index_t trackIndex,
                                              SequencerTrack& track,
                                              AudioProcessContext& ctx) {
            if (!sab_ || trackIndex < 0 || static_cast<uint32_t>(trackIndex) >= kWebAudioMaxTracks)
                return false;
            bool workletHosted = false;
            for (const auto& [instanceId, node] : track.graph().plugins()) {
                (void) instanceId;
                if (node && node->instance() && node->instance()->formatName() == "WebCLAP") {
                    workletHosted = true;
                    break;
                }
            }
            if (!workletHosted || ctx.audioOutBusCount() == 0)
                return false;

            for (uint32_t ch = 0; ch < kWebAudioChannels; ++ch) {
                float* dst = sab_->track_output
                    + (static_cast<size_t>(trackIndex) * kWebAudioChannels + ch) * buffer_size_;
                if (!dst)
                    continue;
                if (ch < static_cast<uint32_t>(ctx.outputChannelCount(0))) {
                    const float* src = ctx.getFloatOutBuffer(0, ch);
                    if (src)
                        std::memcpy(dst, src, buffer_size_ * sizeof(float));
                    else
                        std::memset(dst, 0, buffer_size_ * sizeof(float));
                } else {
                    std::memset(dst, 0, buffer_size_ * sizeof(float));
                }
            }
            return true;
        });
        pump_thread_   = std::thread([this]{ pumpLoop(); });
        engine_thread_ = std::thread([this]{ engineLoop(); });
    }

    void WebAudioEngineThread::stop() {
        if (!running_.load(std::memory_order_relaxed))
            return;
        running_.store(false, std::memory_order_release);
        if (engine_thread_.joinable()) engine_thread_.join();
        if (pump_thread_.joinable())   pump_thread_.join();
        engine_->setTrackOutputHandler({});
        engine_->setExternalPump(false);
    }

    void WebAudioEngineThread::engineLoop() {
        uint32_t last_seq = 0;
        while (running_.load(std::memory_order_acquire)) {
            uint32_t seq = sab_->host_seq.load(std::memory_order_acquire);
            if (seq == last_seq) {
                std::this_thread::yield();
                continue;
            }
            last_seq = seq;

            try {
                std::memset(sab_->track_output,
                            0,
                            static_cast<size_t>(kWebAudioMaxTracks) *
                                kWebAudioChannels *
                                buffer_size_ *
                                sizeof(float));
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

            sab_->track_count.store(static_cast<uint32_t>(std::min<size_t>(engine_->tracks().size(),
                                                                           kWebAudioMaxTracks)),
                                    std::memory_order_release);

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

    // ── EM_JS helpers for the custom AudioWorklet processor ──────────────────
    //
    // A pure-JS AudioWorkletProcessor defined in uapmd-webclap-worklet.js:
    //   • Receives the WASM heap SharedArrayBuffer + SAB byte offset at construction
    //     via processorOptions so it can access WebAudioSAB directly.
    //   • Handles wclap-* control messages (load plugin, unload, configure, etc.).
    //   • In process(): ① runs WebCLAP plugins via wclap.mjs; ② signals host_seq;
    //     ③ spin-waits engine_seq; ④ copies master_output → outputs[].
    //
    // The Emscripten AudioContext is still created the usual way so that
    // emscripten_destroy_audio_context() works in the destructor.

    // Load uapmd-webclap-worklet.js, create the AudioWorkletNode, and connect it
    // to the AudioContext destination.  Called from start() after the AudioContext
    // is created.  sabByteOffset is the byte address of WebAudioSAB inside the
    // Emscripten WASM heap; engineQuantum is buffer_size_.
    EM_JS(void, uapmd_load_webclap_worklet,
          (EMSCRIPTEN_WEBAUDIO_T ctx,
           int sabByteOffset,
           int engineQuantum,
           int hostSeqOffset,
           int engineSeqOffset,
           int trackCountOffset,
           int masterOutOffset,
           int trackOutOffset),
    {
        var audioCtx = emscriptenGetAudioObject(ctx);
        audioCtx.audioWorklet.addModule('uapmd-webclap-worklet.js').then(function() {
            var node = new AudioWorkletNode(audioCtx, 'uapmd-webclap', {
                numberOfInputs:     0,
                numberOfOutputs:    1,
                outputChannelCount: [2],
                processorOptions: {
                    // Pass the Emscripten WASM heap SharedArrayBuffer and the byte
                    // offset of the WebAudioSAB struct so the processor can access
                    // SAB counters and audio buffers via Int32Array/Float32Array views.
                    heapBuffer:     HEAPU8.buffer,
                    sabByteOffset:  sabByteOffset,
                    engineQuantum:  engineQuantum,
                    offsets: {
                        hostSeq: hostSeqOffset,
                        engineSeq: engineSeqOffset,
                        trackCount: trackCountOffset,
                        masterOut: masterOutOffset,
                        trackOut: trackOutOffset,
                    }
                }
            });
            node.connect(audioCtx.destination);
            // Forward messages from the worklet processor to the C++ handler
            // uapmd_webclap_on_worklet_message (defined in PluginFormatWebCLAP.cpp).
            node.port.onmessage = function(e) {
                if (Module._uapmdEnsureWebclapUiManager) {
                    var uiManager = Module._uapmdEnsureWebclapUiManager();
                    if (e.data.type === 'wclap-ui-open') {
                        uiManager.open(e.data.slot, e.data.uri || "", e.data.files || null);
                        return;
                    } else if (e.data.type === 'wclap-ui-message') {
                        uiManager.postToFrame(e.data.slot, e.data.payload);
                        return;
                    }
                }
                var json = JSON.stringify(e.data);
                var len  = lengthBytesUTF8(json) + 1;
                var ptr  = _malloc(len);
                stringToUTF8(json, ptr, len);
                _uapmd_webclap_on_worklet_message(ptr);
                _free(ptr);
            };
            // Compile uapmd-wclap-host.wasm and wasi.wasm on the main thread (fetch and URL are
            // not available inside AudioWorkletGlobalScope) and send them to the worklet.
            // The worklet's wclap-load-plugin handler waits for this message before
            // initialising the host, so plugin loads issued before this resolves are queued.
            // Fetch raw WASM bytes for uapmd-wclap-host and wasi on the main thread
            // (fetch/URL are unavailable in AudioWorkletGlobalScope). Compiled
            // WebAssembly.Module objects cannot cross the AudioWorklet MessagePort
            // in Chrome, so we send ArrayBuffers and compile inside the worklet.
            console.log('[uapmd] Fetching WCLAP host WASM bytes...');
            Promise.all([
                fetch('uapmd-wclap-host.wasm').then(function(r) { return r.arrayBuffer(); }),
                fetch('es6/wasi/wasi.wasm').then(function(r) { return r.arrayBuffer(); }),
            ]).then(function(buffers) {
                console.log('[uapmd] WCLAP host WASM fetched, sending wclap-init-host');
                node.port.postMessage({
                    type:          'wclap-init-host',
                    hostWasmBytes: buffers[0],
                    wasiWasmBytes: buffers[1],
                });
            }).catch(function(err) {
                console.error('[uapmd] Failed to fetch WCLAP host WASM:', err);
            });
            // Store globally so postMessageToWorklet can reach the port.
            // Drain any messages that were queued before the node was ready
            // (e.g. wclap-load-plugin posted before the user started audio).
            Module._wclapWorkletNode = node;
            var pending = Module._wclapPendingMessages;
            if (pending) {
                Module._wclapPendingMessages = null;
                pending.forEach(function(msg) { node.port.postMessage(msg); });
            }
        }).catch(function(err) {
            console.error('[uapmd] Failed to load uapmd-webclap-worklet.js:', err);
        });
    });

    // Post a JSON control message to the AudioWorkletNode's MessagePort.
    // If the node does not exist yet (audio engine not started), the message is
    // queued in Module._wclapPendingMessages and drained when the node is created.
    EM_JS(void, uapmd_post_to_webclap_worklet_json, (const char* json),
    {
        var msg = JSON.parse(UTF8ToString(json));
        var node = Module._wclapWorkletNode;
        if (node) {
            try {
                node.port.postMessage(msg);
            } catch(e) {
                console.error('[uapmd] postMessageToWorklet failed:', e);
            }
        } else {
            if (!Module._wclapPendingMessages)
                Module._wclapPendingMessages = [];
            Module._wclapPendingMessages.push(msg);
        }
    });

    // Fetch the plugin bundle on the main thread (fetch/URL are unavailable in
    // AudioWorkletGlobalScope) and send the raw file bytes to the worklet.
    // Compiled WebAssembly.Module objects cannot cross the AudioWorklet
    // MessagePort in Chrome, so we send ArrayBuffers and compile inside the
    // worklet. For .tar.gz bundles all extracted files are sent; for bare .wasm
    // files the bytes are wrapped in a single-entry map keyed "module.wasm".
    // On error the function reports directly to _uapmd_webclap_on_worklet_message.
    EM_JS(void, uapmd_webclap_load_plugin_async, (const char* json_c),
    {
        var base = JSON.parse(UTF8ToString(json_c));
        var url  = base.url;
        var pluginId = base.pluginId || "";
        var pluginPath = pluginId ? (url + '#plugin=' + pluginId) : url;

        function reportError(err) {
            console.error('[uapmd] load plugin failed:', String(err));
            var errMsg = JSON.stringify({
                type:  'wclap-plugin-error',
                reqId: base.reqId,
                error: String(err),
            });
            var len = lengthBytesUTF8(errMsg) + 1;
            var ptr = _malloc(len);
            stringToUTF8(errMsg, ptr, len);
            _uapmd_webclap_on_worklet_message(ptr);
            _free(ptr);
        }

        function reportParameters(paramDescriptors) {
            var msg = JSON.stringify({
                type: 'wclap-parameters',
                slot: base.slot,
                paramDescriptors: paramDescriptors || [],
            });
            var len = lengthBytesUTF8(msg) + 1;
            var ptr = _malloc(len);
            stringToUTF8(msg, ptr, len);
            _uapmd_webclap_on_worklet_message(ptr);
            _free(ptr);
        }

        function reportUiInfo(uiInfo) {
            var msg = JSON.stringify(Object.assign({
                type: 'wclap-ui-info',
                slot: base.slot,
            }, uiInfo || {}));
            var len = lengthBytesUTF8(msg) + 1;
            var ptr = _malloc(len);
            stringToUTF8(msg, ptr, len);
            _uapmd_webclap_on_worklet_message(ptr);
            _free(ptr);
        }

        function reportCapabilities(capabilities) {
            var msg = JSON.stringify(Object.assign({
                type: 'wclap-capabilities',
                slot: base.slot,
            }, capabilities || {}));
            var len = lengthBytesUTF8(msg) + 1;
            var ptr = _malloc(len);
            stringToUTF8(msg, ptr, len);
            _uapmd_webclap_on_worklet_message(ptr);
            _free(ptr);
        }

        function readCString(memory, ptr) {
            if (!ptr)
                return "";
            var bytes = new Uint8Array(memory.buffer);
            var end = ptr;
            while (end < bytes.length && bytes[end] !== 0)
                end++;
            return new TextDecoder('utf-8').decode(bytes.subarray(ptr, end));
        }

        function buildWclapInit(api, tarFiles) {
            var wasmBuffer = tarFiles['module.wasm'];
            if (!wasmBuffer)
                throw new Error('No module.wasm found in WebCLAP bundle');
            return WebAssembly.compile(wasmBuffer).then(function(wasmModule) {
                var modulePages = Math.max(Math.ceil(wasmBuffer.byteLength / 65536) || 4, 4);
                var memorySpec = { initial: modulePages, maximum: 32768, shared: true };
                return api.getWclap({ url: url, files: tarFiles, module: wasmModule, memorySpec });
            }).then(function(wclapInit) {
                var fixedFiles = {};
                Object.entries(wclapInit.files).forEach(function(entry) {
                    var key = entry[0];
                    var val = entry[1];
                    if ((val instanceof ArrayBuffer && val.byteLength === 0) || key.indexOf('/._') >= 0)
                        return;
                    fixedFiles[key.startsWith('/') ? key : '/' + key] = val;
                });
                wclapInit.files = fixedFiles;
                if (!wclapInit.memory && wclapInit.memorySpec)
                    wclapInit.memory = new WebAssembly.Memory(wclapInit.memorySpec);
                wclapInit.pluginPath = pluginPath;
                return wclapInit;
            });
        }

        function ensureInspectorHost() {
            if (!Module._wclapInspectorHostPromise) {
                Module._wclapInspectorHostPromise = import('./wclap.mjs').then(function(api) {
                    return api.getHost('./uapmd-wclap-host.wasm').then(function(hostInit) {
                        return api.startHost(hostInit).then(function(host) {
                            return {
                                getWclap: api.getWclap,
                                host: host,
                            };
                        });
                    });
                });
            }
            return Module._wclapInspectorHostPromise;
        }

        function inspectParameters(tarFiles) {
            return ensureInspectorHost().then(function(api) {
                return buildWclapInit(api, tarFiles).then(function(wclapInit) {
                    return api.host.startWclap(wclapInit, null).then(function(instance) {
                        var exp = api.host.hostInstance.exports;
                        try {
                            var ok = exp._wclapPluginSetup(instance.ptr, 48000, 128, 128);
                            if (!ok)
                                throw new Error('Inspector plugin setup failed');
                            var paramsPtr = exp._wclapDescribeParameters(instance.ptr);
                            var paramsJson = readCString(api.host.hostMemory, paramsPtr);
                            reportParameters(paramsJson ? JSON.parse(paramsJson) : []);
                            var uiPtr = exp._wclapDescribeUi(instance.ptr);
                            var uiJson = readCString(api.host.hostMemory, uiPtr);
                            reportUiInfo(uiJson ? JSON.parse(uiJson) : { hasUi: false });
                            var capabilitiesPtr = exp._wclapDescribeCapabilities(instance.ptr);
                            var capabilitiesJson = readCString(api.host.hostMemory, capabilitiesPtr);
                            reportCapabilities(capabilitiesJson ? JSON.parse(capabilitiesJson) : {});
                        } finally {
                            exp._wclapPluginDestroy(instance.ptr);
                        }
                    });
                });
            }).catch(function(err) {
                console.warn('[uapmd] parameter inspection failed:', err);
            });
        }

        function sendToWorklet(tarFiles) {
            var msg = {
                type:     'wclap-load-plugin',
                reqId:    base.reqId,
                slot:     base.slot,
                url:      url,
                pluginId: pluginId,
                tarFiles: tarFiles,
            };
            var node = Module._wclapWorkletNode;
            if (node) {
                node.port.postMessage(msg);
            } else {
                // Queue for when the worklet node is created.  If the audio
                // engine is currently off, kick it on automatically so the
                // plugin can load without requiring the user to manually press
                // "Audio Engine: On".
                if (!Module._wclapPendingMessages)
                    Module._wclapPendingMessages = [];
                Module._wclapPendingMessages.push(msg);
                if (typeof Module._uapmd_debug_enable_audio_engine === 'function')
                    Module._uapmd_debug_enable_audio_engine(1);
            }
        }

        Promise.all([
            fetch(url),
            import('./es6/targz.mjs'),
        ]).then(function(results) {
            var response   = results[0];
            var expandTarGz = results[1].default;
            if (response.headers.get('Content-Type') === 'application/wasm') {
                return response.arrayBuffer().then(function(bytes) {
                    var tarFiles = { 'module.wasm': bytes };
                    inspectParameters(tarFiles);
                    sendToWorklet(tarFiles);
                });
            }
            return expandTarGz(response).then(function(tarFiles) {
                inspectParameters(tarFiles);
                sendToWorklet(tarFiles);
            });
        }).catch(reportError);
    });

    void WebAudioWorkletIODevice::postMessageToWorklet(const char* json) const {
        uapmd_post_to_webclap_worklet_json(json);
    }

    uapmd_status_t WebAudioWorkletIODevice::start() {
        if (is_playing_)
            return 0;
        is_playing_ = true;

        if (engine_thread_) {
            engine_thread_->start();
            engine_thread_ready_.store(engine_thread_.get(), std::memory_order_release);
        }

        EmscriptenWebAudioCreateAttributes attrs{
            .latencyHint = "interactive",
            .sampleRate  = sample_rate_,
        };
        audio_ctx_ = emscripten_create_audio_context(&attrs);
        emscripten_resume_audio_context_sync(audio_ctx_);

        // Load uapmd-webclap-worklet.js and create the AudioWorkletNode.
        // The processor receives the WASM heap buffer + SAB byte offset via
        // processorOptions and handles audio I/O and WebCLAP plugin processing.
        uapmd_load_webclap_worklet(audio_ctx_,
                                   static_cast<int>(reinterpret_cast<uintptr_t>(&sab_)),
                                   static_cast<int>(buffer_size_),
                                   static_cast<int>(offsetof(WebAudioSAB, host_seq)),
                                   static_cast<int>(offsetof(WebAudioSAB, engine_seq)),
                                   static_cast<int>(offsetof(WebAudioSAB, track_count)),
                                   static_cast<int>(offsetof(WebAudioSAB, master_output)),
                                   static_cast<int>(offsetof(WebAudioSAB, track_output)));
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
