import { getWclap } from './vendor/webclap/wclap.mjs';
import { getWasi, startWasi } from './vendor/webclap/es6/wasi/wasi.mjs';
import generateForwardingWasm from './vendor/webclap/es6/generate-forwarding-wasm.mjs';

const kWasmDirectoryUrl = new URL('.', import.meta.url);
const kAppScriptUrl = new URL('uapmd-app.js', kWasmDirectoryUrl).href;

function getWebclapBridge() {
    return globalThis.uapmdWebClap || null;
}

function createWebclapImportObject(module) {
    const callBridge = (method, defaultValue, ...args) => {
        const bridge = getWebclapBridge();
        if (!bridge || typeof bridge[method] !== 'function') {
            if (module && typeof module.printErr === 'function') {
                module.printErr(`[WebCLAP] Bridge method ${method} is unavailable.`);
            }
            return defaultValue;
        }
        try {
            return bridge[method](...args);
        } catch (error) {
            if (module && typeof module.printErr === 'function') {
                module.printErr(`[WebCLAP] Bridge method ${method} threw ${error}`);
            }
            return defaultValue;
        }
    };
    return {
        runThread(handle, threadId, ctxLow) {
            callBridge('runThread', undefined, handle, threadId, ctxLow);
        },
        release(handle) {
            callBridge('releaseInstance', undefined, handle);
        },
        init32(handle) {
            return callBridge('init32', 0, handle) | 0;
        },
        init64(handle) {
            return callBridge('init64', 0, handle) | 0;
        },
        malloc32(handle, size) {
            return callBridge('malloc32', 0, handle, size) | 0;
        },
        malloc64(handle, lo, hi) {
            return callBridge('malloc64', 0, handle, lo, hi) | 0;
        },
        memcpyToOther32(handle, dest, src, size) {
            return callBridge('memcpyToOther32', 0, handle, dest, src, size) | 0;
        },
        memcpyToOther64(handle, dest, src) {
            return callBridge('memcpyToOther64', 0, handle, dest, src) | 0;
        },
        memcpyFromOther32(handle, dest, src, size) {
            return callBridge('memcpyFromOther32', 0, handle, dest, src, size) | 0;
        },
        memcpyFromOther64(handle, dest, src) {
            return callBridge('memcpyFromOther64', 0, handle, dest, src) | 0;
        },
        countUntil32(handle, startPtr, untilPtr, itemSize, maxCount) {
            return callBridge('countUntil32', 0, handle, startPtr, untilPtr, itemSize, maxCount) | 0;
        },
        countUntil64(handle, startPtr, untilPtr, itemSize, maxCount) {
            return callBridge('countUntil64', 0, handle, startPtr, untilPtr, itemSize, maxCount) | 0;
        },
        call32(handle, wasmFn, isPtrToFn, resultPtr, argsPtr, argsCount) {
            return callBridge('call32', 0, handle, wasmFn, !!isPtrToFn, resultPtr, argsPtr, argsCount) | 0;
        },
        call64(handle, wasmFn, isPtrToFn, resultPtr, argsPtr, argsCount) {
            return callBridge('call64', 0, handle, wasmFn, !!isPtrToFn, resultPtr, argsPtr, argsCount) | 0;
        },
        registerHost32(handle, context, fnIndex, sigPtr, sigLength) {
            return callBridge('registerHost32', 0, handle, context, fnIndex, sigPtr, sigLength) | 0;
        },
        registerHost64(handle, context, fnIndex, sigPtr, sigLength) {
            return callBridge('registerHost64', 0, handle, context, fnIndex, sigPtr, sigLength) | 0;
        }
    };
}

function resolveWasmUrl(module) {
    const defaultUrl = new URL('uapmd-app.wasm', kWasmDirectoryUrl).href;
    if (module && typeof module.locateFile === 'function') {
        try {
            return module.locateFile('uapmd-app.wasm', kWasmDirectoryUrl.href) || defaultUrl;
        } catch (error) {
            if (module.printErr) {
                module.printErr(`[WebCLAP] locateFile threw: ${error}`);
            }
        }
    }
    return defaultUrl;
}

function installInstantiateHook(module) {
    if (!module || module.__webclapInstantiateHookInstalled) return;
    module.__webclapInstantiateHookInstalled = true;

    const previous = module.instantiateWasm;
    if (typeof previous === 'function') {
        module.instantiateWasm = (imports, successCallback) => {
            imports._wclapInstance = createWebclapImportObject(module);
            const wrappedCallback = typeof successCallback === 'function'
                ? (instance, mod) => {
                    rememberWasmTable(module, instance);
                    successCallback(instance, mod);
                }
                : successCallback;
            return previous(imports, wrappedCallback);
        };
        return;
    }

    module.instantiateWasm = (imports, successCallback) => {
        imports._wclapInstance = createWebclapImportObject(module);
        const wasmUrl = resolveWasmUrl(module);

        const runSuccess = (instance, mod) => {
            rememberWasmTable(module, instance);
            if (typeof successCallback === 'function') {
                successCallback(instance, mod);
            }
        };

        const instantiateFromBinary = async (binary) => {
            const result = await WebAssembly.instantiate(binary, imports);
            runSuccess(result.instance, result.module);
        };

        const instantiateFromFetch = async () => {
            const response = await fetch(wasmUrl, { credentials: 'same-origin' });
            let streamingFailed = false;
            if (WebAssembly.instantiateStreaming) {
                try {
                    const streamingResult = await WebAssembly.instantiateStreaming(response.clone(), imports);
                    runSuccess(streamingResult.instance, streamingResult.module);
                    return;
                } catch (error) {
                    streamingFailed = true;
                    if (module.printErr) {
                        module.printErr(`[WebCLAP] wasm streaming compile failed: ${error}`);
                    }
                }
            }
            if (streamingFailed && module.printErr) {
                module.printErr('[WebCLAP] Falling back to ArrayBuffer instantiation.');
            }
            const buffer = await response.arrayBuffer();
            await instantiateFromBinary(buffer);
        };

        (async () => {
            if (module.wasmBinary) {
                await instantiateFromBinary(module.wasmBinary);
            } else {
                await instantiateFromFetch();
            }
        })().catch(error => {
            if (module.printErr) {
                module.printErr(`[WebCLAP] instantiateWasm failed: ${error}`);
            }
            throw error;
        });
        return {};
    };
}

function rememberWasmTable(module, instance) {
    if (!module || !instance || !instance.exports) return;
    const table = instance.exports.__indirect_function_table;
    if (table instanceof WebAssembly.Table) {
        module.__webclapWasmTable = table;
    }
    const memory = instance.exports.memory;
    if (memory instanceof WebAssembly.Memory) {
        module.__webclapMemory = memory;
    }
}

installInstantiateHook(globalThis.Module || (globalThis.Module = {}));

class UapmdWebClapBridge {
    constructor(module) {
        this.module = module;
        this.instances = new Map();
        this.functionLabels = new Map();
        this.textEncoder = new TextEncoder();
        this.traceProcess = !!globalThis.__uapmdWebClapTraceProcess;
        this.traceProcessCount = 0;
        this.hostReady = new Promise(resolve => {
            const prev = module.onRuntimeInitialized;
            module.onRuntimeInitialized = () => {
                if (prev) prev();
                this._onRuntimeInitialized();
                resolve();
            };
        });
    }

    _onRuntimeInitialized() {
        this.wasmTable =
            this.module.__webclapWasmTable ||
            (this.module.asm && this.module.asm.__indirect_function_table) ||
            this.module.wasmTable ||
            null;
        if (!(this.wasmTable instanceof WebAssembly.Table)) {
            this.wasmTable = null;
            console.error('[WebCLAP] Failed to locate the wasm indirect function table; host functions will not work.');
        }
        this._hostDataView = null;
        this._heapCache = null;
        this._ensureNativeExports();
    }

    _hostView() {
        const heap = this._heapU8();
        if (!this._hostDataView || this._hostDataView.buffer !== heap.buffer) {
            this._hostDataView = new DataView(heap.buffer);
        }
        return this._hostDataView;
    }

    _heapU8() {
        const direct = this._resolveDirectHeap();
        if (direct) {
            this._heapCache = direct;
            return direct;
        }
        const memory = this.module.__webclapMemory;
        if (memory instanceof WebAssembly.Memory) {
            if (!this._heapCache || this._heapCache.buffer !== memory.buffer) {
                this._heapCache = new Uint8Array(memory.buffer);
            }
            return this._heapCache;
        }
        throw new Error('[WebCLAP] wasm heap is unavailable; export HEAPU8 or memory.');
    }

    _resolveDirectHeap() {
        const desc = Object.getOwnPropertyDescriptor(this.module, 'HEAPU8');
        if (desc && Object.prototype.hasOwnProperty.call(desc, 'value') && desc.value instanceof Uint8Array) {
            return desc.value;
        }
        return null;
    }

    _ensureNativeExports() {
        const alias = (publicName, fallbackName) => {
            if (typeof this.module[publicName] === 'function') {
                return true;
            }
            const fallback =
                this.module[fallbackName] ||
                this.module.asm?.[fallbackName] ||
                globalThis[fallbackName];
            if (typeof fallback === 'function') {
                this.module[publicName] = fallback;
                return true;
            }
            return false;
        };

        const required = [
            ['_wclapInstanceCreate', '__wclapInstanceCreate'],
            ['_wclapInstanceSetPath', '__wclapInstanceSetPath'],
            ['_wclapInstanceGetNextIndex', '__wclapInstanceGetNextIndex'],
            ['_wclapNextThreadId', '__wclapNextThreadId'],
            ['_wclapStartInstanceThread', '__wclapStartInstanceThread'],
            ['_malloc', '_malloc'],
            ['_free', '_free']
        ];

        const missing = [];
        required.forEach(([publicName, fallbackName]) => {
            if (!alias(publicName, fallbackName)) {
                missing.push(publicName);
            }
        });

        if (missing.length && typeof this.module.printErr === 'function') {
            this.module.printErr(`[WebCLAP] Missing wasm exports: ${missing.join(', ')}`);
        }
    }

    async _ensureWasiRoot() {
        if (!this.wasiRootPromise) {
            const init = await getWasi();
            this.wasiRootPromise = startWasi(init);
        }
        return this.wasiRootPromise;
    }

    async requestInstance(token, bundleUrl, pluginId) {
        try {
            await this.hostReady;
            const initObj = await getWclap({
                url: bundleUrl,
                pluginPath: `/browser/webclap/${token}/${encodeURIComponent(pluginId)}`
            });
            const record = await this._startWclap(initObj);
            this._notifyReady(token, true, record.ptr, initObj.pluginPath);
        } catch (error) {
            const details = error && error.stack ? error.stack : error;
            console.warn('[WebCLAP] requestInstance failed:', details);
            this._notifyReady(token, false, 0, error?.message || 'Failed to prepare WebCLAP bundle.');
        }
    }

    async _startWclap(initObj) {
        const imports = {};
        if (globalThis.__uapmdWebClapTraceProcess) {
            const keys = Object.keys(initObj);
            console.warn('[WebCLAP][trace] initObj keys:', keys.join(','));
            if (initObj.functionTable instanceof WebAssembly.Table) {
                console.warn('[WebCLAP][trace] initObj functionTable length:', initObj.functionTable.length);
            }
        }
        const needsInit = !Object.prototype.hasOwnProperty.call(initObj, 'instancePtr');
        let instancePtr = needsInit ? 0 : initObj.instancePtr >>> 0;
        const sharedArrayBufferCtor = typeof SharedArrayBuffer !== 'undefined' ? SharedArrayBuffer : null;
        const moduleImports = WebAssembly.Module.imports(initObj.module);
        let needsWasi = false;
        const memoryImports = [];
        for (const entry of moduleImports) {
            if (/^wasi/.test(entry.module)) needsWasi = true;
            if (entry.kind === 'memory') {
                memoryImports.push(entry);
            }
        }

        let pluginMemory = initObj.memory || null;
        if (memoryImports.length) {
            const requiresSharedMemory = memoryImports.some(entry => entry.shared);
            pluginMemory = this._resolvePluginMemory(initObj, requiresSharedMemory, sharedArrayBufferCtor);
            memoryImports.forEach(entry => {
                if (!imports[entry.module]) imports[entry.module] = {};
                imports[entry.module][entry.name] = pluginMemory;
            });
        }

        let pluginWasi = null;
        if (needsWasi) {
            const wasiRoot = await this._ensureWasiRoot();
            pluginWasi = await wasiRoot.copyForRebinding();
            Object.assign(imports, pluginWasi.importObj);
            if (needsInit && initObj.files && Object.keys(initObj.files).length) {
                pluginWasi.loadFiles(initObj.files);
            }
        }

        imports.wasi = imports.wasi || {};
        imports.wasi['thread-spawn'] = threadArg => this._pluginThreadSpawn(instancePtr, threadArg);

        const pluginInstance = await WebAssembly.instantiate(initObj.module, imports);
        const functionTable = this._findFunctionTable(pluginInstance);
        const is64 = pluginInstance.exports.clap_entry instanceof BigInt;
        if (is64) {
            throw new Error('wasm64 WebCLAP modules are not supported yet.');
        }

        if (needsInit) {
            instancePtr = this.module._wclapInstanceCreate(is64 ? 1 : 0);
            if (!instancePtr) {
                throw new Error('WebCLAP host failed to allocate an instance handle.');
            }
        }

        const entry = {
            instance: pluginInstance,
            memory: pluginMemory || pluginInstance.exports.memory,
            functionTable,
            hostFunctions: initObj.hostFunctions ? { ...initObj.hostFunctions } : {},
            hadInit: !needsInit,
            pluginWasi,
            createWorkerFn: initObj.createWorkerFn || null,
            is64,
            shared: !!(sharedArrayBufferCtor && pluginMemory && pluginMemory.buffer instanceof sharedArrayBufferCtor),
            configuration: null,
            active: false,
            processWarningShown: false,
            initObj,
            hostFunctionMeta: {}
        };
        if (pluginWasi) {
            pluginWasi.bindToOtherMemory(entry.memory);
        }
        this.instances.set(instancePtr, entry);
        if (!needsInit) {
            this._assignHostFunctions(entry);
        } else {
            this._writePluginPath(instancePtr, initObj.pluginPath || `/browser/webclap/${instancePtr}`);
        }
        return { ptr: instancePtr };
    }

    _findFunctionTable(pluginInstance) {
        for (const key of Object.keys(pluginInstance.exports)) {
            const candidate = pluginInstance.exports[key];
            if (candidate instanceof WebAssembly.Table) {
                if (candidate.length > 0 && typeof candidate.get(candidate.length - 1) === 'function') {
                    return candidate;
                }
            }
        }
        throw new Error('[WebCLAP] Plugin did not export a function table.');
    }

    _writePluginPath(instancePtr, pluginPath) {
        if (!pluginPath) return;
        const encoded = this.textEncoder.encode(pluginPath);
        const ptr = this.module._wclapInstanceSetPath(instancePtr, encoded.length);
        if (!ptr) return;
        const heap = this._heapU8();
        heap.set(encoded, ptr);
    }

    _resolvePluginMemory(initObj, requiresSharedMemory, sharedArrayBufferCtor) {
        if (initObj.memory) {
            const isShared = !!sharedArrayBufferCtor && initObj.memory.buffer instanceof sharedArrayBufferCtor;
            if (requiresSharedMemory && !isShared) {
                throw new Error('[WebCLAP] Plugin requires SharedArrayBuffer, but the provided memory is not shared.');
            }
            return initObj.memory;
        }
        const spec = initObj.memorySpec ? { ...initObj.memorySpec } : { initial: 8, maximum: 32768 };
        if (requiresSharedMemory) {
            spec.shared = true;
        }
        try {
            return new WebAssembly.Memory(spec);
        } catch (error) {
            if (spec.shared) {
                throw new Error('[WebCLAP] This plugin requires SharedArrayBuffer. Serve the page with COOP/COEP headers to enable it.');
            }
            throw error;
        }
    }

    _getEntry(instancePtr) {
        const entry = this.instances.get(instancePtr);
        if (!entry) throw new Error(`WebCLAP handle ${instancePtr} is not registered.`);
        return entry;
    }

    configureInstance(instancePtr, sampleRate, blockSize, mainInputs, mainOutputs) {
        const entry = this._getEntry(instancePtr);
        entry.configuration = {
            sampleRate,
            blockSize,
            mainInputs,
            mainOutputs
        };
        entry.active = false;
        return true;
    }

    startInstance(instancePtr) {
        const entry = this._getEntry(instancePtr);
        entry.active = true;
        return true;
    }

    stopInstance(instancePtr) {
        const entry = this._getEntry(instancePtr);
        entry.active = false;
        return true;
    }

    processInstance(instancePtr, frameCount) {
        const entry = this._getEntry(instancePtr);
        if (!entry || !entry.active) return 0;
        if (!this.module || typeof this.module.ccall !== 'function') {
            return -1;
        }
        const status = this.module.ccall(
            'uapmd_webclap_native_process',
            'number',
            ['number', 'number'],
            [instancePtr >>> 0, frameCount >>> 0]
        ) | 0;
        return status;
    }

    releaseInstance(instancePtr) {
        this.instances.delete(instancePtr);
    }

    runThread(instancePtr, threadId, threadContext) {
        const entry = this._getEntry(instancePtr);
        if (!entry.hadInit) throw new Error('runThread called before init.');
        const contextArg = entry.is64 ? threadContext : Number(threadContext >>> 0);
        entry.instance.exports.wasi_thread_start(threadId, contextArg);
    }

    init32(instancePtr) {
        const entry = this._getEntry(instancePtr);
        if (entry.hadInit) throw new Error('WCLAP already initialised.');
        entry.hadInit = true;
        this._assignHostFunctions(entry);
        if (typeof entry.instance.exports._initialize === 'function') {
            entry.instance.exports._initialize();
        }
        return entry.instance.exports.clap_entry;
    }

    init64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    malloc32(instancePtr, size) {
        const entry = this._getEntry(instancePtr);
        if (typeof entry.instance.exports.malloc !== 'function') {
            throw new Error('WCLAP module does not export malloc().');
        }
        return entry.instance.exports.malloc(size);
    }

    malloc64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    memcpyToOther32(instancePtr, wclapPtr, hostPtr, size) {
        const entry = this._getEntry(instancePtr);
        const hostHeap = this._heapU8();
        new Uint8Array(entry.memory.buffer, wclapPtr, size)
            .set(hostHeap.subarray(hostPtr, hostPtr + size));
        return 1;
    }

    memcpyToOther64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    memcpyFromOther32(instancePtr, hostPtr, wclapPtr, size) {
        const entry = this._getEntry(instancePtr);
        const hostHeap = this._heapU8();
        hostHeap.subarray(hostPtr, hostPtr + size)
            .set(new Uint8Array(entry.memory.buffer, wclapPtr, size));
        return 1;
    }

    memcpyFromOther64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    countUntil32(instancePtr, startPtr, untilPtr, itemSize, maxCount) {
        const entry = this._getEntry(instancePtr);
        const hostHeap = this._heapU8();
        const stopper = new Uint8Array(hostHeap.buffer, untilPtr, itemSize);
        const buffer = new Uint8Array(entry.memory.buffer);
        for (let i = 0; i < maxCount; ++i) {
            let different = false;
            for (let b = 0; b < itemSize; ++b) {
                if (buffer[startPtr + i * itemSize + b] !== stopper[b]) {
                    different = true;
                    break;
                }
            }
            if (!different) return i;
        }
        return maxCount;
    }

    countUntil64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    call32(instancePtr, wasmFn, isPtrToFn, resultPtr, argsPtr, argsCount) {
        const entry = this._getEntry(instancePtr);
        const memoryView = new DataView(entry.memory.buffer);
        let targetIndex = wasmFn;
        if (isPtrToFn) {
            targetIndex = memoryView.getUint32(wasmFn, true);
        }

        const args = [];
        const hostView = this._hostView();
        for (let i = 0; i < argsCount; ++i) {
            const ptr = argsPtr + i * 16;
            const type = hostView.getUint8(ptr);
            if (type === 0) {
                args.push(hostView.getInt32(ptr + 8, true));
            } else if (type === 1) {
                args.push(hostView.getBigUint64(ptr + 8, true));
            } else if (type === 2) {
                args.push(hostView.getFloat32(ptr + 8, true));
            } else if (type === 3) {
                args.push(hostView.getFloat64(ptr + 8, true));
            }
        }

        const fn = entry.functionTable.get(targetIndex);
        const label = this.functionLabels.get(targetIndex) || 'unknown';
        if (this.traceProcess && label === 'clap_plugin.process' && this.traceProcessCount < 16) {
            const summary = args.map((value, idx) => `${idx}:${typeof value === 'bigint' ? `${value}n` : value}`).join(', ');
            this.module.printErr?.(`[WebCLAP][trace] call32 ${label} table[${targetIndex}] args=[${summary}] resultPtr=${resultPtr}`);
            this.traceProcessCount++;
        }
        if (typeof fn !== 'function') throw new Error('Invalid function pointer from WCLAP.');
        let result;
        try {
            result = fn(...args);
        } catch (error) {
            if (this.module && typeof this.module.printErr === 'function') {
                const meta = entry.hostFunctionMeta ? entry.hostFunctionMeta[targetIndex] : null;
                const sigLabel = meta?.sig || 'unknown';
                const hostIndex = meta?.hostIndex ?? 'unknown';
                const fnArity = typeof fn === 'function' ? fn.length : 'n/a';
                const argSummary = args.map((value, idx) => `${idx}:${typeof value}`).join(', ');
                this.module.printErr(`[WebCLAP] call32 failed at table[${targetIndex}] (hostIdx=${hostIndex}, sig=${sigLabel}, label=${label}, fnLen=${fnArity}, args=[${argSummary}]): ${error}`);
            }
            throw error;
        }

        if (result === undefined || result === null) {
            hostView.setUint8(resultPtr, 0);
            hostView.setUint32(resultPtr + 8, 0, true);
        } else if (typeof result === 'boolean') {
            hostView.setUint8(resultPtr, 0);
            hostView.setUint32(resultPtr + 8, result ? 1 : 0, true);
        } else if (typeof result === 'number') {
            if ((result | 0) === result) {
                hostView.setUint8(resultPtr, 0);
                hostView.setInt32(resultPtr + 8, result, true);
            } else {
                hostView.setUint8(resultPtr, 3);
                hostView.setFloat64(resultPtr + 8, result, true);
            }
        } else if (typeof result === 'bigint') {
            hostView.setUint8(resultPtr, 1);
            hostView.setBigUint64(resultPtr + 8, result, true);
        } else {
            throw new Error('Unsupported return type from host function.');
        }
        return 1;
    }

    call64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    registerHost32(instancePtr, context, fnIndex, sigPtr, sigLength) {
        const entry = this._getEntry(instancePtr);
        if (entry.hadInit) {
            throw new Error('Cannot register host functions after init().');
        }
        const signature = this._readAscii(sigPtr, sigLength);
        if (!entry.functionTable) throw new Error('Plugin missing function table.');
        const wasmFnIndex = entry.functionTable.length;
        entry.functionTable.grow(1);
        entry.hostFunctions['hostFn' + fnIndex] = {
            instanceIndex: wasmFnIndex,
            hostIndex: fnIndex,
            context,
            sig: signature
        };
        return wasmFnIndex;
    }

    registerHost64() {
        throw new Error('wasm64 WebCLAP is not supported.');
    }

    registerFunctionLabel(index, label) {
        if (!Number.isFinite(index) || index <= 0) {
            return;
        }
        const normalized = String(label || '');
        if (!normalized.length) {
            return;
        }
        this.functionLabels.set(index >>> 0, normalized);
    }

    _assignHostFunctions(entry) {
        const hostFnEntries = entry.hostFunctions;
        if (!hostFnEntries || Object.keys(hostFnEntries).length === 0) return;

        const fnSigs = {};
        const imports = { proxy: {} };
        for (const [key, fnEntry] of Object.entries(hostFnEntries)) {
            fnSigs[key] = fnEntry.sig;
            const hostFn = this.wasmTable.get(fnEntry.hostIndex);
            imports.proxy[key] = hostFn.bind(null, fnEntry.context);
        }
        const wasm = generateForwardingWasm(fnSigs);
        const forwardingInstance = new WebAssembly.Instance(new WebAssembly.Module(wasm), imports);
        for (const [key, fnEntry] of Object.entries(hostFnEntries)) {
            if (entry.functionTable.length <= fnEntry.instanceIndex) {
                entry.functionTable.grow(fnEntry.instanceIndex + 1 - entry.functionTable.length);
            }
            entry.functionTable.set(fnEntry.instanceIndex, forwardingInstance.exports[key]);
            if (!entry.hostFunctionMeta) entry.hostFunctionMeta = {};
            entry.hostFunctionMeta[fnEntry.instanceIndex] = {
                sig: fnEntry.sig,
                hostIndex: fnEntry.hostIndex
            };
        }
    }

    _pluginThreadSpawn(_, threadArg) {
        console.warn('[WebCLAP] Plugin attempted to spawn a thread, but thread support is not yet implemented.', threadArg);
        return -1;
    }

    _readAscii(ptr, length) {
        if (!length) return '';
        const bytes = this._heapU8().subarray(ptr, ptr + length);
        return String.fromCharCode(...bytes);
    }

    _notifyReady(token, success, instancePtr, message) {
        const payloadPtr = message ? this._allocCString(message) : 0;
        this.module.ccall('uapmd_webclap_instance_ready', 'void',
            ['number', 'number', 'number', 'number'],
            [token, success ? 1 : 0, instancePtr >>> 0, payloadPtr]);
        if (payloadPtr) this.module._free(payloadPtr);
    }

    _allocCString(text) {
        const encoded = this.textEncoder.encode(text);
        const ptr = this.module._malloc(encoded.length + 1);
        const heap = this._heapU8();
        heap.set(encoded, ptr);
        heap[ptr + encoded.length] = 0;
        return ptr;
    }
}

function installBridge() {
    if (!globalThis.Module) {
        console.error('[WebCLAP] Module object is unavailable; cannot install bridge.');
        return;
    }
    globalThis.uapmdWebClap = new UapmdWebClapBridge(globalThis.Module);
}

function ensureMainScriptLoaded() {
    if (globalThis.__uapmdMainScriptLoaded) {
        return;
    }
    if (typeof document === 'undefined') {
        console.error('[WebCLAP] Document is unavailable; cannot boot the wasm shell.');
        return;
    }

    const inject = () => {
        if (globalThis.__uapmdMainScriptLoaded) {
            return;
        }
        const existing = document.querySelector('script[data-uapmd-app-main]');
        if (existing) {
            globalThis.__uapmdMainScriptLoaded = true;
            return;
        }
        const script = document.createElement('script');
        script.src = kAppScriptUrl;
        script.async = false;
        script.defer = false;
        script.dataset.uapmdAppMain = 'true';
        script.addEventListener('error', (event) => {
            console.error('[WebCLAP] Failed to load uapmd-app.js', event?.error || event);
        });
        (document.body || document.documentElement).appendChild(script);
        globalThis.__uapmdMainScriptLoaded = true;
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', inject, { once: true });
    } else {
        inject();
    }
}

if (globalThis.Module) {
    installBridge();
} else {
    globalThis.Module = {};
    installBridge();
}

ensureMainScriptLoaded();
