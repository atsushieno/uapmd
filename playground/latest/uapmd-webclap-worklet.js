// uapmd-webclap-worklet.js
//
// Custom AudioWorkletProcessor driving audio I/O and WebCLAP plugin processing.
//
// SAB memory layout (WebAudioSAB in WebAudioWorkletIODevice.hpp):
//   Offset  Size     Name
//   0       4        host_seq    (uint32, AudioWorklet → engine pthread)
//   4       4        engine_seq  (uint32, engine pthread → AudioWorklet)
//   8       1024     audio_input (float[2][128], device → engine)
//   1032    varies   master_output (float[2][engineQuantum], engine → device)
//
// Per-quantum flow when _accumCount === 0:
//   ① _wclapPluginProcess for each active slot → localOut in host WASM
//   ② Signal engine: Atomics.store(host_seq, n+1)
//   ③ Spin-wait engine_seq === n+1
//   ④ Mix localOut into master_output (in-place add)
// Then always: copy master_output[_accumCount] slice → outputs[0].

import { startHost, getWclap } from './wclap.mjs';

const kChannels = 2;
const kQuantum  = 128;

class UapmdWebclapProcessor extends AudioWorkletProcessor {

    constructor(options) {
        super();
        const { heapBuffer, sabByteOffset, engineQuantum, offsets } = options.processorOptions;

        // Views over the Emscripten (uapmd-app) WASM heap SharedArrayBuffer.
        this._i32 = new Int32Array(heapBuffer);
        this._f32 = new Float32Array(heapBuffer);

        this._hostSeqIdx    = (sabByteOffset + offsets.hostSeq) >> 2;
        this._engineSeqIdx  = (sabByteOffset + offsets.engineSeq) >> 2;
        this._trackCountIdx = (sabByteOffset + offsets.trackCount) >> 2;
        this._masterOutIdx  = (sabByteOffset + offsets.masterOut) >> 2;
        this._trackOutIdx   = (sabByteOffset + offsets.trackOut) >> 2;
        this._engineQuantum  = engineQuantum;
        this._quantaPerRender= engineQuantum / kQuantum;
        this._accumCount     = 0;

        // WebCLAP state
        this._slots            = new Map();   // slot → {ptr, active, outLOff, outROff, inLOff, inROff}
        this._trackGraphs      = new Map();   // trackIndex → [slot]
        this._masterGraph      = [];
        this._wclapHost        = null;
        this._wclapHostPromise = null;
        this._hostF32          = null;        // Float32Array over uapmd-wclap-host.wasm memory
        this._workL            = new Float32Array(engineQuantum);
        this._workR            = new Float32Array(engineQuantum);
        this._scratchL         = new Float32Array(engineQuantum);
        this._scratchR         = new Float32Array(engineQuantum);

        // Compiled WebAssembly.Module objects for uapmd-wclap-host.wasm and wasi.wasm,
        // sent from the main thread via wclap-init-host (fetch is unavailable here).
        this._wclapHostModule  = null;
        this._wclapWasiModule  = null;
        // Resolves when wclap-init-host has delivered the compiled host modules.
        this._wclapInitResolve = null;
        this._wclapInitPromise = new Promise(r => { this._wclapInitResolve = r; });

        this.port.onmessage = (e) => {
            this._handleMessage(e.data).catch((err) => {
                console.error('[uapmd-webclap-worklet] message error:', err);
            });
        };
    }

    _ensureHostViews() {
        if (!this._wclapHost)
            return null;
        if (!this._hostF32 || this._hostF32.buffer !== this._wclapHost.hostMemory.buffer)
            this._hostF32 = new Float32Array(this._wclapHost.hostMemory.buffer);
        return new Uint8Array(this._wclapHost.hostMemory.buffer);
    }

    _readCString(ptr) {
        if (!ptr)
            return '';
        const bytes = this._ensureHostViews();
        if (!bytes)
            return '';
        let end = ptr;
        while (end < bytes.length && bytes[end] !== 0)
            end++;
        let text = '';
        for (let i = ptr; i < end; ++i)
            text += String.fromCharCode(bytes[i]);
        return text;
    }

    _drainUiMessages(slot, info) {
        if (!this._wclapHost || !info)
            return;
        const exp = this._wclapHost.hostInstance.exports;
        while (exp._wclapUiHasOutgoingMessage(info.ptr)) {
            const size = exp._wclapUiDequeueOutgoingMessage(info.ptr);
            if (!size)
                break;
            const ptr = exp._wclapUiGetOutgoingMessageData(info.ptr);
            const bytes = new Uint8Array(this._wclapHost.hostMemory.buffer, ptr, size);
            const payload = new Uint8Array(size);
            payload.set(bytes);
            this.port.postMessage({ type: 'wclap-ui-message', slot, payload: payload.buffer }, [payload.buffer]);
        }
        if (exp._wclapUiHasPendingResize(info.ptr)) {
            const ptr = exp._wclapUiTakeResizeRequest(info.ptr);
            const resize = this._readCString(ptr);
            if (resize) {
                const parsed = JSON.parse(resize);
                this.port.postMessage({
                    type: 'wclap-ui-resize',
                    slot,
                    hasUi: true,
                    canResize: !!parsed.canResize,
                    width: parsed.width || exp._wclapUiGetWidth(info.ptr),
                    height: parsed.height || exp._wclapUiGetHeight(info.ptr),
                });
            }
        }
        while (exp._wclapHasParameterUpdates(info.ptr)) {
            const ptr = exp._wclapTakeParameterUpdates(info.ptr);
            const updates = this._readCString(ptr);
            if (!updates)
                break;
            this.port.postMessage({
                type: 'wclap-parameter-updates',
                slot,
                updates: JSON.parse(updates),
            });
        }
    }

    _trackBaseIndex(trackIndex) {
        return this._trackOutIdx + trackIndex * kChannels * this._engineQuantum;
    }

    _removeSlotFromGraphs(slot) {
        for (const [trackIndex, slots] of this._trackGraphs.entries()) {
            const filtered = slots.filter(id => id !== slot);
            if (filtered.length)
                this._trackGraphs.set(trackIndex, filtered);
            else
                this._trackGraphs.delete(trackIndex);
        }
        this._masterGraph = this._masterGraph.filter(id => id !== slot);
    }

    _replaceInputBuffer(info, srcL, srcR) {
        if (!this._hostF32 || !info || !info.inLOff || !info.inROff)
            return;
        const dstL = info.inLOff >> 2;
        const dstR = info.inROff >> 2;
        this._hostF32.set(srcL, dstL);
        this._hostF32.set(srcR, dstR);
    }

    _runTrackGraph(slotIds, srcBaseIdx, accumulateIntoMaster) {
        if (!this._wclapHost || slotIds.length === 0)
            return;
        const eq = this._engineQuantum;
        const exp = this._wclapHost.hostInstance.exports;

        this._workL.set(this._f32.subarray(srcBaseIdx, srcBaseIdx + eq));
        this._workR.set(this._f32.subarray(srcBaseIdx + eq, srcBaseIdx + eq * 2));

        for (const slot of slotIds) {
            const info = this._slots.get(slot);
            if (!info || !info.active)
                continue;
            this._replaceInputBuffer(info, this._workL, this._workR);
            exp._wclapPluginProcess(info.ptr, this._engineQuantum);
            const srcL = info.outLOff >> 2;
            const srcR = info.outROff >> 2;
            this._workL.set(this._hostF32.subarray(srcL, srcL + eq));
            this._workR.set(this._hostF32.subarray(srcR, srcR + eq));
        }

        if (accumulateIntoMaster) {
            const dstL = this._masterOutIdx;
            const dstR = this._masterOutIdx + eq;
            for (let i = 0; i < eq; ++i) {
                this._f32[dstL + i] += this._workL[i];
                this._f32[dstR + i] += this._workR[i];
            }
        } else {
            this._f32.set(this._workL, this._masterOutIdx);
            this._f32.set(this._workR, this._masterOutIdx + eq);
        }
    }

    // ── Control messages ──────────────────────────────────────────────────────

    async _handleMessage(msg) {
        switch (msg.type) {

            case 'wclap-init-host': {
                // Compile the raw WASM bytes received from the main thread.
                // WebAssembly.Module cannot cross the AudioWorklet MessagePort in
                // Chrome, so the main thread sends ArrayBuffers and we compile here.
                console.log('[uapmd-webclap-worklet] wclap-init-host received, compiling...');
                const [hostModule, wasiModule] = await Promise.all([
                    WebAssembly.compile(msg.hostWasmBytes),
                    WebAssembly.compile(msg.wasiWasmBytes),
                ]);
                this._wclapHostModule = hostModule;
                this._wclapWasiModule = wasiModule;
                this._wclapInitResolve();
                console.log('[uapmd-webclap-worklet] wclap-init-host: host modules ready');
                this.port.postMessage({ type: 'wclap-host-ready' });
                break;
            }

            case 'wclap-load-plugin': {
                const { reqId, slot, url, pluginId, tarFiles } = msg;
                try {
                    // Lazy host initialisation (shared across all slots).
                    // Waits for wclap-init-host to deliver the compiled modules because
                    // fetch and URL are not available in AudioWorkletGlobalScope.
                    if (!this._wclapHostPromise) {
                        this._wclapHostPromise = (async () => {
                            await this._wclapInitPromise;
                            this._wclapHost = await startHost({
                                module: this._wclapHostModule,
                                wasi:   { module: this._wclapWasiModule },
                            });
                        })();
                    }
                    await this._wclapHostPromise;

                    // Pre-compile the plugin WASM here so getWclap receives a
                    // WebAssembly.Module (not an ArrayBuffer).  The ArrayBuffer code
                    // path in getWclap has a reference to an undefined variable
                    // ('module' instead of 'options.module') that throws in strict ES
                    // module scope; passing a pre-compiled module avoids that branch.
                    const wasmBuffer = tarFiles['module.wasm'];
                    const wasmModule = await WebAssembly.compile(wasmBuffer);

                    // Compute a memory descriptor for plugins that import memory.
                    // Heuristic: use the compiled WASM size (in pages) as the initial
                    // size, matching the logic in wclap-plugin.mjs's guessMemorySize.
                    const modulePages = Math.max(Math.ceil(wasmBuffer.byteLength / 65536) || 4, 4);
                    // Use shared:true so wasi-threads plugins (which import shared memory) work.
                    // The page has COOP/COEP so SharedArrayBuffer is available.
                    const memorySpec = { initial: modulePages, maximum: 32768, shared: true };

                    const wclapInit = await getWclap({ url, files: tarFiles, module: wasmModule, memorySpec });
                    wclapInit.pluginPath = pluginId ? `${url}#plugin=${pluginId}` : url;

                    // Rebuild the file map so every path starts with '/' (required by
                    // WASI vfs_createFile) while also dropping:
                    //  • 0-byte ArrayBuffers inserted by getWclap ("avoid self-parsing")
                    //  • macOS resource-fork files (._*) not needed by the plugin
                    const fixedFiles = {};
                    for (const [key, val] of Object.entries(wclapInit.files)) {
                        if ((val instanceof ArrayBuffer && val.byteLength === 0) ||
                            /\/\._/.test(key))
                            continue;
                        const fixedKey = key.startsWith('/') ? key : '/' + key;
                        fixedFiles[fixedKey] = val;
                    }
                    wclapInit.files = fixedFiles;

                    const { ptr } = await this._wclapHost.startWclap(wclapInit, null);
                    this._slots.set(slot, {
                        ptr,
                        active: false,
                        outLOff: 0,
                        outROff: 0,
                        inLOff: 0,
                        inROff: 0,
                        pluginId: pluginId || '',
                        files: fixedFiles,
                    });

                    this.port.postMessage({ type: 'wclap-plugin-ready', reqId, slot });
                } catch (err) {
                    console.error('[uapmd-webclap-worklet] load failed:', err.stack || err);
                    this.port.postMessage({ type: 'wclap-plugin-error', reqId,
                                            error: String(err) });
                }
                break;
            }

            case 'wclap-configure': {
                const { slot, sampleRate, bufferSize } = msg;
                const info = this._slots.get(slot);
                if (!info || !this._wclapHost) break;
                try {
                    const exp = this._wclapHost.hostInstance.exports;

                    const ok = exp._wclapPluginSetup(
                        info.ptr, sampleRate, bufferSize, bufferSize);
                    if (!ok) {
                        const error = `Plugin setup failed for slot ${slot}`;
                        console.error('[uapmd-webclap-worklet]', error);
                        this.port.postMessage({ type: 'wclap-runtime-error', slot, error });
                        break;
                    }

                    // Cache buffer offsets after non-realtime setup succeeds.
                    info.inLOff = exp._wclapGetInputL(info.ptr);
                    info.inROff = exp._wclapGetInputR(info.ptr);
                    info.outLOff = exp._wclapGetOutputL(info.ptr);
                    info.outROff = exp._wclapGetOutputR(info.ptr);

                    if (!this._hostF32)
                        this._hostF32 = new Float32Array(this._wclapHost.hostMemory.buffer);
                } catch (err) {
                    console.error('[uapmd-webclap-worklet] configure failed:', err.stack || err);
                    this.port.postMessage({
                        type: 'wclap-runtime-error',
                        slot,
                        error: String(err),
                    });
                }
                break;
            }

            case 'wclap-start': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                this._wclapHost.hostInstance.exports._wclapPluginStartProcessing(info.ptr);
                info.active = true;
                break;
            }

            case 'wclap-stop': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                this._wclapHost.hostInstance.exports._wclapPluginStopProcessing(info.ptr);
                info.active = false;
                break;
            }

            case 'wclap-unload': {
                const info = this._slots.get(msg.slot);
                if (info && this._wclapHost) {
                    this._wclapHost.hostInstance.exports._wclapPluginDestroy(info.ptr);
                }
                this._slots.delete(msg.slot);
                this._removeSlotFromGraphs(msg.slot);
                break;
            }

            case 'wclap-graph-add-node': {
                this._removeSlotFromGraphs(msg.slot);
                if (msg.isMaster) {
                    const order = Math.max(0, Math.min(msg.order ?? this._masterGraph.length, this._masterGraph.length));
                    this._masterGraph.splice(order, 0, msg.slot);
                } else {
                    const slots = this._trackGraphs.get(msg.trackIndex) || [];
                    const order = Math.max(0, Math.min(msg.order ?? slots.length, slots.length));
                    slots.splice(order, 0, msg.slot);
                    this._trackGraphs.set(msg.trackIndex, slots);
                }
                break;
            }

            case 'wclap-ui-create': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const exp = this._wclapHost.hostInstance.exports;
                const ok = exp._wclapUiCreate(info.ptr, msg.width || 800, msg.height || 600);
                if (!ok)
                    break;
                this.port.postMessage({
                    type: 'wclap-ui-open',
                    slot: msg.slot,
                    hasUi: true,
                    canResize: !!exp._wclapUiCanResize(info.ptr),
                    width: exp._wclapUiGetWidth(info.ptr),
                    height: exp._wclapUiGetHeight(info.ptr),
                    uri: this._readCString(exp._wclapUiGetUri(info.ptr)),
                    files: info.files,
                });
                this._drainUiMessages(msg.slot, info);
                break;
            }

            case 'wclap-ui-show': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                this._wclapHost.hostInstance.exports._wclapUiShow(info.ptr);
                this._drainUiMessages(msg.slot, info);
                break;
            }

            case 'wclap-ui-hide': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                this._wclapHost.hostInstance.exports._wclapUiHide(info.ptr);
                break;
            }

            case 'wclap-ui-destroy': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                this._wclapHost.hostInstance.exports._wclapUiDestroy(info.ptr);
                break;
            }

            case 'wclap-ui-set-size': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const exp = this._wclapHost.hostInstance.exports;
                if (exp._wclapUiSetSize(info.ptr, msg.width || 0, msg.height || 0)) {
                    this.port.postMessage({
                        type: 'wclap-ui-resize',
                        slot: msg.slot,
                        hasUi: true,
                        canResize: !!exp._wclapUiCanResize(info.ptr),
                        width: exp._wclapUiGetWidth(info.ptr),
                        height: exp._wclapUiGetHeight(info.ptr),
                    });
                }
                this._drainUiMessages(msg.slot, info);
                break;
            }

            case 'wclap-ui-from-frame': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const exp = this._wclapHost.hostInstance.exports;
                const ptr = exp._wclapUiMessageBufferPtr(info.ptr);
                const cap = exp._wclapUiMessageBufferCapacity(info.ptr);
                if (!ptr || !cap) break;
                let bytes;
                if (msg.payload instanceof ArrayBuffer)
                    bytes = new Uint8Array(msg.payload);
                else if (ArrayBuffer.isView(msg.payload))
                    bytes = new Uint8Array(msg.payload.buffer, msg.payload.byteOffset, msg.payload.byteLength);
                else {
                    const payload = typeof msg.payload === 'string' ? msg.payload : JSON.stringify(msg.payload);
                    bytes = new TextEncoder().encode(payload);
                }
                const hostBytes = new Uint8Array(this._wclapHost.hostMemory.buffer);
                const size = Math.min(bytes.length, cap - 1);
                hostBytes.set(bytes.subarray(0, size), ptr);
                hostBytes[ptr + size] = 0;
                exp._wclapUiReceiveMessage(info.ptr, size);
                this._drainUiMessages(msg.slot, info);
                break;
            }

            case 'wclap-set-parameter': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const exp = this._wclapHost.hostInstance.exports;
                exp._wclapEnqueueParameterValue(
                    info.ptr,
                    msg.index,
                    msg.value);
                exp._wclapFlushParameters(info.ptr);
                this._drainUiMessages(msg.slot, info);
                break;
            }

            case 'wclap-send-ump': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const words = Array.isArray(msg.words) ? msg.words : [];
                this._wclapHost.hostInstance.exports._wclapEnqueueMidi2Event(
                    info.ptr,
                    words[0] || 0,
                    words[1] || 0,
                    words[2] || 0,
                    words[3] || 0);
                break;
            }

            case 'wclap-send-ump-batch': {
                const info = this._slots.get(msg.slot);
                if (!info || !this._wclapHost) break;
                const events = Array.isArray(msg.events) ? msg.events : [];
                const exp = this._wclapHost.hostInstance.exports;
                for (const event of events) {
                    const words = Array.isArray(event?.words) ? event.words : [];
                    exp._wclapEnqueueMidi2Event(
                        info.ptr,
                        words[0] || 0,
                        words[1] || 0,
                        words[2] || 0,
                        words[3] || 0);
                }
                break;
            }

            default:
                console.warn('[uapmd-webclap-worklet] unknown message:', msg.type);
        }
    }

    // ── Audio processing ──────────────────────────────────────────────────────

    process(_inputs, outputs) {
        const i32 = this._i32;
        const f32 = this._f32;

        if (this._accumCount === 0) {
            // ① Signal the engine pthread to render native/dry track stems.
            const seq = Atomics.load(i32, this._hostSeqIdx) + 1;
            Atomics.store(i32, this._hostSeqIdx, seq);

            // ② Spin-wait for engine to finish writing master_output + track stems.
            while (Atomics.load(i32, this._engineSeqIdx) !== seq) { /* spin */ }

            // ③ Run worklet-owned WebCLAP graphs using dry track stems from the engine.
            if (this._hostF32) {
                const trackCount = Atomics.load(i32, this._trackCountIdx);
                for (let trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
                    const slots = this._trackGraphs.get(trackIndex);
                    if (!slots || slots.length === 0)
                        continue;
                    this._runTrackGraph(slots, this._trackBaseIndex(trackIndex), true);
                }
                if (this._masterGraph.length > 0)
                    this._runTrackGraph(this._masterGraph, this._masterOutIdx, false);
                for (const [slot, info] of this._slots.entries())
                    this._drainUiMessages(slot, info);
            }
        }

        // Copy master_output slice _accumCount → outputs[0].
        const out = outputs[0];
        if (out) {
            const chCount = Math.min(out.length, kChannels);
            for (let ch = 0; ch < chCount; ch++) {
                const srcOff = this._masterOutIdx
                    + ch * this._engineQuantum
                    + this._accumCount * kQuantum;
                out[ch].set(f32.subarray(srcOff, srcOff + kQuantum));
            }
        }

        if (++this._accumCount >= this._quantaPerRender)
            this._accumCount = 0;

        return true;
    }
}

registerProcessor('uapmd-webclap', UapmdWebclapProcessor);
