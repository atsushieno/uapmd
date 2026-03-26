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

    EM_JS(void, uapmd_ensure_webclap_bridge, (), {
        Module._uapmdEnsureWebclapBridge = function() {
            if (Module._uapmdWebclapBridge)
                return Module._uapmdWebclapBridge;
            const bridge = {
                node: null,
                pending: [],
                uiManager: null,
                parameterFormatters: new Map(),
                nextRequestId: 1,
                pendingRpc: new Map(),
                inspectorHostPromise: null,
                setNode(node) {
                    this.node = node;
                    Module._wclapWorkletNode = node;
                    const queued = this.pending;
                    this.pending = [];
                    queued.forEach(entry => this.post(entry.message, entry.transfers));
                },
                ensureUiManager() {
                    if (this.uiManager)
                        return this.uiManager;
                    const bridgeRef = this;
                    this.uiManager = {
                        bindings: new Map(),
                        bind(slot, containerId) {
                            const binding = { slot, containerId, iframe: null, uri: "", bodyId: containerId };
                            this.bindings.set(slot, binding);
                        },
                        unbind(slot) {
                            const binding = this.bindings.get(slot);
                            if (binding && binding.blobUrls)
                                Object.values(binding.blobUrls).forEach(function(url) { URL.revokeObjectURL(url); });
                            if (binding && binding.iframe)
                                binding.iframe.remove();
                            this.bindings.delete(slot);
                        },
                        getBody(slot) {
                            const binding = this.bindings.get(slot);
                            if (!binding)
                                return null;
                            return document.getElementById(binding.containerId);
                        },
                        mimeFor(path) {
                            if (path.endsWith('.html')) return 'text/html';
                            if (path.endsWith('.js') || path.endsWith('.mjs')) return 'text/javascript';
                            if (path.endsWith('.css')) return 'text/css';
                            if (path.endsWith('.svg')) return 'image/svg+xml';
                            if (path.endsWith('.json')) return 'application/json';
                            if (path.endsWith('.wasm')) return 'application/wasm';
                            return 'application/octet-stream';
                        },
                        findFileKey(files, uri) {
                            if (!files)
                                return null;
                            let normalized = uri;
                            if (normalized.startsWith('file://'))
                                normalized = normalized.slice(7);
                            else if (normalized.startsWith('file:'))
                                normalized = normalized.slice(5);
                            if (files[normalized]) return normalized;
                            if (files['/' + normalized]) return '/' + normalized;
                            for (const key of Object.keys(files))
                                if (normalized.endsWith(key) || key.endsWith(normalized))
                                    return key;
                            return null;
                        },
                        open(slot, uri, files) {
                            const binding = this.bindings.get(slot);
                            const body = this.getBody(slot);
                            if (!binding || !body)
                                return;
                            if (binding.blobUrls) {
                                Object.values(binding.blobUrls).forEach(function(url) { URL.revokeObjectURL(url); });
                                binding.blobUrls = null;
                            }
                            if (binding.iframe)
                                binding.iframe.remove();
                            body.dataset.webclapSlot = String(slot);
                            body.textContent = "";
                            const iframe = document.createElement('iframe');
                            iframe.id = `uapmd-webclap-frame-${slot}`;
                            iframe.dataset.webclapSlot = String(slot);
                            iframe.style.border = '0';
                            iframe.style.width = '100%';
                            iframe.style.height = '100%';
                            iframe.style.background = '#111';
                            body.appendChild(iframe);
                            binding.iframe = iframe;
                            binding.uri = uri;
                            if ((uri.startsWith('file:') || uri.startsWith('/')) && files) {
                                const fileKey = this.findFileKey(files, uri);
                                if (fileKey) {
                                    const root = files[fileKey];
                                    const blobUrls = {};
                                    for (const [path, content] of Object.entries(files)) {
                                        const blob = new Blob([content], { type: this.mimeFor(path) });
                                        blobUrls[path] = URL.createObjectURL(blob);
                                    }
                                    if (fileKey.endsWith('.html')) {
                                        let html = new TextDecoder('utf-8').decode(root);
                                        for (const [path, blobUrl] of Object.entries(blobUrls)) {
                                            const basename = path.split('/').pop();
                                            html = html.split(path).join(blobUrl);
                                            if (basename)
                                                html = html.split(basename).join(blobUrl);
                                        }
                                        iframe.srcdoc = html;
                                    } else {
                                        iframe.src = blobUrls[fileKey];
                                    }
                                    binding.blobUrls = blobUrls;
                                } else {
                                    iframe.src = uri;
                                }
                            } else {
                                iframe.src = uri;
                            }
                        },
                        postToFrame(slot, payload) {
                            const binding = this.bindings.get(slot);
                            if (!binding || !binding.iframe || !binding.iframe.contentWindow)
                                return;
                            if (payload instanceof ArrayBuffer) {
                                binding.iframe.contentWindow.postMessage(payload, '*', [payload]);
                                return;
                            }
                            if (ArrayBuffer.isView(payload)) {
                                binding.iframe.contentWindow.postMessage(payload.buffer, '*', [payload.buffer]);
                                return;
                            }
                            let value = payload;
                            if (typeof payload === 'string') {
                                try { value = JSON.parse(payload); } catch (_) {}
                            }
                            binding.iframe.contentWindow.postMessage(value, '*');
                        },
                    };
                    window.addEventListener('message', function(event) {
                        for (const [slot, binding] of bridgeRef.uiManager.bindings.entries()) {
                            if (!binding.iframe || event.source !== binding.iframe.contentWindow)
                                continue;
                            const payload = event.data;
                            if (payload instanceof ArrayBuffer)
                                bridgeRef.postRpc(-1, 'uiFromFrame', [slot, payload], [payload]);
                            else
                                bridgeRef.postRpc(-1, 'uiFromFrame', [slot, payload], null);
                            break;
                        }
                    });
                    return this.uiManager;
                },
                releaseParameterFormatter(slot) {
                    const formatter = this.parameterFormatters.get(slot);
                    if (!formatter)
                        return;
                    this.parameterFormatters.delete(slot);
                    try {
                        formatter.api.host.hostInstance.exports._wclapPluginDestroy(formatter.instance.ptr);
                    } catch (e) {
                        console.warn('[uapmd] failed to release WebCLAP formatter:', e);
                    }
                },
                rememberParameterFormatter(slot, api, instance) {
                    this.releaseParameterFormatter(slot);
                    this.parameterFormatters.set(slot, { api, instance });
                },
                formatParameterValue(slot, index, value) {
                    const formatter = this.parameterFormatters.get(slot);
                    if (!formatter)
                        return "";
                    try {
                        const ptr = formatter.api.host.hostInstance.exports._wclapFormatParameterValue(formatter.instance.ptr, index, value);
                        return ptr ? this.readCString(formatter.api.host.hostMemory, ptr) : "";
                    } catch (e) {
                        console.warn('[uapmd] failed to format WebCLAP parameter value:', e);
                        return "";
                    }
                },
                bindUiSlot(slot, containerId) {
                    this.ensureUiManager().bind(slot, containerId);
                },
                unbindUiSlot(slot) {
                    if (this.uiManager)
                        this.uiManager.unbind(slot);
                },
                post(message, transfers) {
                    if (this.node) {
                        try {
                            if (transfers && transfers.length)
                                this.node.port.postMessage(message, transfers);
                            else
                                this.node.port.postMessage(message);
                        } catch (e) {
                            console.error('[uapmd] postMessageToWorklet failed:', e);
                        }
                        return;
                    }
                    this.pending.push({ message: message, transfers: transfers || null });
                },
                postJson(message) {
                    this.post(message, null);
                },
                postRpc(requestId, method, args, transfers) {
                    if (method === 'unload' && args && args.length > 0)
                        this.releaseParameterFormatter(args[0]);
                    this.post([requestId, method, args], transfers);
                },
                callRpc(method, args, transfers) {
                    const requestId = this.nextRequestId++;
                    return new Promise((resolve, reject) => {
                        this.pendingRpc.set(requestId, { resolve, reject });
                        this.postRpc(requestId, method, args, transfers);
                    });
                },
                forwardJsonToHost(data) {
                    var json = JSON.stringify(data);
                    var len  = lengthBytesUTF8(json) + 1;
                    var ptr  = _malloc(len);
                    stringToUTF8(json, ptr, len);
                    _uapmd_webclap_on_worklet_message(ptr);
                    _free(ptr);
                },
                readCString(memory, ptr) {
                    if (!ptr)
                        return "";
                    var bytes = new Uint8Array(memory.buffer);
                    var end = ptr;
                    while (end < bytes.length && bytes[end] !== 0)
                        end++;
                    return new TextDecoder('utf-8').decode(bytes.subarray(ptr, end));
                },
                reportParameters(slot, paramDescriptors) {
                    this.forwardJsonToHost({
                        type: 'webclap-parameter-descriptors',
                        slot: slot,
                        paramDescriptors: paramDescriptors || [],
                    });
                },
                reportUiInfo(slot, uiInfo) {
                    this.forwardJsonToHost(Object.assign({
                        type: 'webclap-ui-descriptor',
                        slot: slot,
                    }, uiInfo || {}));
                },
                reportCapabilities(slot, capabilities) {
                    this.forwardJsonToHost(Object.assign({
                        type: 'webclap-capabilities',
                        slot: slot,
                    }, capabilities || {}));
                },
                reportScanResult(reqId, plugins) {
                    this.forwardJsonToHost({
                        type: 'webclap-scan-complete',
                        reqId: reqId,
                        plugins: plugins || [],
                    });
                },
                reportScanError(reqId, err) {
                    this.forwardJsonToHost({
                        type: 'webclap-scan-failed',
                        reqId: reqId,
                        error: String(err),
                    });
                },
                ensureInspectorHost() {
                    if (!this.inspectorHostPromise) {
                        this.inspectorHostPromise = import('./wclap.mjs').then(function(api) {
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
                    return this.inspectorHostPromise;
                },
                buildWclapInit(base, tarFiles, api) {
                    var url = base.url;
                    var pluginId = base.pluginId || "";
                    var pluginPath = pluginId ? (url + '#plugin=' + pluginId) : url;
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
                },
                inspectParameters(base, tarFiles) {
                    var bridge = this;
                    return this.ensureInspectorHost().then(function(api) {
                        bridge.releaseParameterFormatter(base.slot);
                        return bridge.buildWclapInit(base, tarFiles, api).then(function(wclapInit) {
                            return api.host.startWclap(wclapInit, null).then(function(instance) {
                                var exp = api.host.hostInstance.exports;
                                var keepFormatter = false;
                                try {
                                    var ok = exp._wclapPluginSetup(instance.ptr, 48000, 128, 128);
                                    if (!ok)
                                        throw new Error('Inspector plugin setup failed');
                                    var paramsPtr = exp._wclapDescribeParameters(instance.ptr);
                                    var paramsJson = bridge.readCString(api.host.hostMemory, paramsPtr);
                                    bridge.reportParameters(base.slot, paramsJson ? JSON.parse(paramsJson) : []);
                                    var uiPtr = exp._wclapDescribeUi(instance.ptr);
                                    var uiJson = bridge.readCString(api.host.hostMemory, uiPtr);
                                    bridge.reportUiInfo(base.slot, uiJson ? JSON.parse(uiJson) : { hasUi: false });
                                    var capabilitiesPtr = exp._wclapDescribeCapabilities(instance.ptr);
                                    var capabilitiesJson = bridge.readCString(api.host.hostMemory, capabilitiesPtr);
                                    bridge.reportCapabilities(base.slot, capabilitiesJson ? JSON.parse(capabilitiesJson) : {});
                                    bridge.rememberParameterFormatter(base.slot, api, instance);
                                    keepFormatter = true;
                                } finally {
                                    if (!keepFormatter)
                                        exp._wclapPluginDestroy(instance.ptr);
                                }
                            });
                        });
                    }).catch(function(err) {
                        console.warn('[uapmd] parameter inspection failed:', err);
                    });
                },
                inspectDescriptors(base, tarFiles) {
                    var bridge = this;
                    return this.ensureInspectorHost().then(function(api) {
                        return bridge.buildWclapInit(base, tarFiles, api).then(function(wclapInit) {
                            return api.host.startWclap(wclapInit, null).then(function(instance) {
                                var exp = api.host.hostInstance.exports;
                                var pluginsPtr = exp._wclapDescribePlugins(instance.ptr);
                                var pluginsJson = bridge.readCString(api.host.hostMemory, pluginsPtr);
                                bridge.reportScanResult(base.reqId, pluginsJson ? JSON.parse(pluginsJson) : []);
                            });
                        });
                    }).catch(function(err) {
                        bridge.reportScanError(base.reqId, err);
                    });
                },
                instantiatePlugin(base, tarFiles) {
                    var bridge = this;
                    var url = base.url;
                    var pluginId = base.pluginId || "";
                    var rpcTarFiles = {};
                    var transfers = [];
                    Object.entries(tarFiles).forEach(function(entry) {
                        var key = entry[0];
                        var content = entry[1];
                        if (content instanceof ArrayBuffer) {
                            var clone = content.slice(0);
                            rpcTarFiles[key] = clone;
                            transfers.push(clone);
                        } else {
                            rpcTarFiles[key] = content;
                        }
                    });
                    return bridge.callRpc('loadPlugin', [base.slot, url, pluginId, rpcTarFiles], transfers)
                        .then(function() {
                            bridge.forwardJsonToHost({
                                type: 'webclap-instance-created',
                                reqId: base.reqId,
                                slot: base.slot,
                            });
                        })
                        .catch(function(err) {
                            bridge.forwardJsonToHost({
                                type: 'webclap-instance-create-failed',
                                reqId: base.reqId,
                                error: String(err),
                            });
                        });
                },
                loadBundleRequest(base) {
                    var bridge = this;
                    var url = base.url;
                    function reportError(err) {
                        console.error('[uapmd] load plugin failed:', String(err));
                        bridge.forwardJsonToHost({
                            type: base.type === 'webclap-scan' ? 'webclap-scan-failed' : 'webclap-instance-create-failed',
                            reqId: base.reqId,
                            error: String(err),
                        });
                    }
                    return Promise.all([
                        fetch(url),
                        import('./es6/targz.mjs'),
                    ]).then(function(results) {
                        var response = results[0];
                        var expandTarGz = results[1].default;
                        function handleTarFiles(tarFiles) {
                            if (base.type === 'webclap-scan')
                                return bridge.inspectDescriptors(base, tarFiles);
                            bridge.inspectParameters(base, tarFiles);
                            if (!bridge.node && typeof Module._uapmd_debug_enable_audio_engine === 'function')
                                Module._uapmd_debug_enable_audio_engine(1);
                            return bridge.instantiatePlugin(base, tarFiles);
                        }
                        if (response.headers.get('Content-Type') === 'application/wasm') {
                            return response.arrayBuffer().then(function(bytes) {
                                return handleTarFiles({ 'module.wasm': bytes });
                            });
                        }
                        return expandTarGz(response).then(handleTarFiles);
                    }).catch(reportError);
                },
                handleMessage(data) {
                    if (Array.isArray(data) && typeof data[0] === 'number') {
                        const requestId = data[0];
                        const pending = this.pendingRpc.get(requestId);
                        if (!pending)
                            return;
                        this.pendingRpc.delete(requestId);
                        if (data[1])
                            pending.reject(data[1]);
                        else
                            pending.resolve(data[2]);
                        return;
                    }
                    if (data.type === 'webclap-ui-opened') {
                        this.ensureUiManager().open(data.slot, data.uri || "", data.files || null);
                        return;
                    } else if (data.type === 'bridge-ui-message') {
                        this.ensureUiManager().postToFrame(data.slot, data.payload);
                        return;
                    } else if (data.type === 'bridge-ui-resize') {
                        this.forwardJsonToHost(Object.assign({}, data, { type: 'webclap-ui-resized' }));
                        return;
                    } else if (data.type === 'bridge-parameter-updates') {
                        this.forwardJsonToHost(Object.assign({}, data, { type: 'webclap-parameter-values-updated' }));
                        return;
                    }
                    this.forwardJsonToHost(data);
                },
            };
            Module._uapmdWebclapBridge = bridge;
            return bridge;
        };
        Module._uapmdEnsureWebclapBridge();
    });

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
        Module._uapmdEnsureWebclapBridge();
        var audioCtx = emscriptenGetAudioObject(ctx);
        audioCtx.audioWorklet.addModule('uapmd-webclap-worklet.js').then(function() {
            var bridge = Module._uapmdEnsureWebclapBridge();
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
            node.port.onmessage = function(e) {
                bridge.handleMessage(e.data);
            };
            // Compile uapmd-wclap-host.wasm and wasi.wasm on the main thread (fetch and URL are
            // not available inside AudioWorkletGlobalScope) and send them to the worklet.
            // The bridge's loadPlugin RPC path waits for this bootstrap before
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
            bridge.setNode(node);
        }).catch(function(err) {
            console.error('[uapmd] Failed to load uapmd-webclap-worklet.js:', err);
        });
    });

    EM_JS(void, uapmd_post_to_webclap_worklet_rpc, (const char* method, const char* args_json),
    {
        Module._uapmdEnsureWebclapBridge().postRpc(-1, UTF8ToString(method), JSON.parse(UTF8ToString(args_json)), null);
    });

    EM_JS(void, uapmd_webclap_create_ui_rpc, (uint32_t slot, uint32_t width, uint32_t height),
    {
        var bridge = Module._uapmdEnsureWebclapBridge();
        bridge.callRpc('createUi', [slot, width, height], null).then(function(uiInfo) {
            if (!uiInfo)
                return;
            bridge.ensureUiManager().open(uiInfo.slot, uiInfo.uri || "", uiInfo.files || null);
            bridge.forwardJsonToHost(Object.assign({ type: 'webclap-ui-opened' }, uiInfo));
        }).catch(function(err) {
            console.error('[uapmd] createUi RPC failed:', err);
        });
    });

    EM_JS(void, uapmd_webclap_set_ui_size_rpc, (uint32_t slot, uint32_t width, uint32_t height),
    {
        var bridge = Module._uapmdEnsureWebclapBridge();
        bridge.callRpc('setUiSize', [slot, width, height], null).then(function(uiInfo) {
            if (!uiInfo)
                return;
            bridge.forwardJsonToHost(Object.assign({}, uiInfo, { type: 'webclap-ui-resized' }));
        }).catch(function(err) {
            console.error('[uapmd] setUiSize RPC failed:', err);
        });
    });

    EM_JS(void, uapmd_webclap_request_state_rpc, (uint32_t reqId, uint32_t slot, uint32_t stateContextType),
    {
        var bridge = Module._uapmdEnsureWebclapBridge();
        bridge.callRpc('requestState', [slot, stateContextType], null).then(function(payload) {
            var stateBytes = payload instanceof ArrayBuffer ? new Uint8Array(payload) : new Uint8Array(0);
            var payloadPtr = 0;
            var payloadSize = stateBytes.byteLength;
            if (payloadSize > 0) {
                payloadPtr = _malloc(payloadSize);
                HEAPU8.set(stateBytes, payloadPtr);
            }
            _uapmd_webclap_on_worklet_state_response(reqId >>> 0, payloadPtr, payloadSize, 0);
            if (payloadPtr)
                _free(payloadPtr);
        }).catch(function(err) {
            var errorPtr = 0;
            if (err != null) {
                var errorText = String(err);
                var errorLen = lengthBytesUTF8(errorText) + 1;
                errorPtr = _malloc(errorLen);
                stringToUTF8(errorText, errorPtr, errorLen);
            }
            _uapmd_webclap_on_worklet_state_response(reqId >>> 0, 0, 0, errorPtr);
            if (errorPtr)
                _free(errorPtr);
        });
    });

    EM_JS(void, uapmd_webclap_load_state_rpc,
          (uint32_t reqId, uint32_t slot, uint32_t stateContextType, const uint8_t* data, size_t size),
    {
        var payload = new ArrayBuffer(0);
        var transfers = null;
        if (size > 0 && data) {
            var bytes = HEAPU8.slice(data, data + size);
            payload = bytes.buffer;
            transfers = [payload];
        }
        var bridge = Module._uapmdEnsureWebclapBridge();
        bridge.callRpc('loadState', [slot, payload, stateContextType], transfers).then(function() {
            _uapmd_webclap_on_worklet_state_load_complete(reqId >>> 0, 0);
        }).catch(function(err) {
            var errorPtr = 0;
            if (err != null) {
                var errorText = String(err);
                var errorLen = lengthBytesUTF8(errorText) + 1;
                errorPtr = _malloc(errorLen);
                stringToUTF8(errorText, errorPtr, errorLen);
            }
            _uapmd_webclap_on_worklet_state_load_complete(reqId >>> 0, errorPtr);
            if (errorPtr)
                _free(errorPtr);
        });
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
        Module._uapmdEnsureWebclapBridge().loadBundleRequest(JSON.parse(UTF8ToString(json_c)));
    });

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

        uapmd_ensure_webclap_bridge();
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
