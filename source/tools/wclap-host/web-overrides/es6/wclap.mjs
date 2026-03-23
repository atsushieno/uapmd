import {getWasi, startWasi} from "./wasi/wasi.mjs";
import getWclap from "./wclap-plugin.mjs";
import generateForwardingWasm from "./generate-forwarding-wasm.mjs"

/* These exported functions should work for any Wasm32 host using the `wclap-js-instance` version of `Instance`.*/
export {getHost, startHost, getWclap, runThread};

class WclapHost {
	#config;
	#wasi;
	#wclapMap = Object.create(null);
	#functionTable;
	#hostCallbackOrder = [
		'_wclapHostGetExt',
		'_wclapHostReqRestart',
		'_wclapHostReqProcess',
		'_wclapHostReqCallback',
		'_wclapInEventsSize',
		'_wclapInEventsGet',
		'_wclapOutEventsTryPush',
	];
	
	ready;
	hostInstance;
	hostMemory;
	shared;

	#threadSpawnInstancePtr = null;
	#hostThreadSpawnWASIT1(hostThreadContext) {
		let hostThreadId = this.hostInstance.exports._wclapNextThreadId();
		let instancePtr = this.#threadSpawnInstancePtr;
		this.#threadSpawnInstancePtr = null;
		if (instancePtr == null) throw "Only instances can spawn threads (not the host)";
		
		let entry = this.#wclapMap[instancePtr];
		if (!entry) throw Error("Starting thread from unknown Instance: " + instancePtr);
		let createWorker = entry.createWorkerFn;
		if (!createWorker) {
			console.error("WCLAP was not started with thread support");
			return -1;
		}

		// The data passed here is minimal enough that it can always be cloned (even from a Worklet)
		let created = createWorker(this, {
			instancePtr: instancePtr,
			threadId: hostThreadId,
			threadContext: hostThreadContext,
			hostFunctions: entry.hostFunctions
		});
		if (!created) {
			console.error("Host failed to create a WCLAP thread Worker");
			return -1;
		}
		
		return hostThreadId;
	}
	#pluginThreadSpawnWASIT1(instancePtr, threadArg) {
		this.#threadSpawnInstancePtr = instancePtr;
		// This call makes the host start a new thread, which will (synchronously) call #hostThreadSpawn above, and pick up the config
		return this.hostInstance.exports._wclapStartInstanceThread(instancePtr, BigInt(threadArg));
	}
	getWorkerData(wclapInit, threadData) {
		if (!globalThis.crossOriginIsolated) {
			throw Error("Trying to start thread from invalid context (not cross-origin isolated)");
		}
		if (!wclapInit.memory) {
			console.error("Plugin attempted to start a new thread, but has no shared memory");
			debugger;
			return -1;
		}

		// This is what `runWorker()` actually needs
		return {
			instancePtr: threadData.instancePtr,
			threadId: threadData.threadId,
			threadContext: threadData.threadContext,
			hostFunctions: threadData.hostFunctions,

			host: this.initObj(),
			wclap: wclapInit
		};
	}
	static async runWorker(workerData, hostImports, createWorkerFn) {
		let host = await startHost(workerData.host, hostImports);
		let wclapInit = Object.assign({
			instancePtr: workerData.instancePtr,
			hostFunctions: workerData.hostFunctions,
		}, workerData.wclap);
		let wclap = await host.startWclap(wclapInit, createWorkerFn);
		let entry = host.#wclapMap[wclapInit.instancePtr];
		if (!entry) throw Error("Worker started, but `Instance *` isn't in the map");
		host.hostInstance.exports.wasi_thread_start(workerData.threadId, workerData.threadContext);
	}

	constructor(config, hostImports) {
		if (!hostImports) hostImports = {};
		// Methods which let the host manage the WCLAP in another module
		let getEntry = instancePtr => {
			let entry = this.#wclapMap[instancePtr];
			if (!entry) throw Error("tried to address Instance which isn't in #wclapMap");
			return entry;
		};
		// When `WebAssembly.Function` is widely supported, we can avoid this workaround: https://github.com/WebAssembly/js-types/blob/main/proposals/js-types/Overview.md#addition-of-webassemblyfunction

		// These are the methods declared in `wclap-js-instance.h`, to provide the `Instance` implementation
		hostImports._wclapInstance = {
			release: instancePtr => {
				delete this.#wclapMap[instancePtr];
			},
			runThread: (instancePtr, threadId, threadContext) => {
				let entry = getEntry(instancePtr);
				if (!entry.hadInit) throw Error(".runThread() called before initialisation");

				if (!entry.is64) threadContext = Number(threadContext); // it's a pointer, which we previous cast to uint64_t, but this is a 32-bit WCLAP
				entry.instance.exports.wasi_thread_start(threadId, threadContext);
			},
			// wclap32
			registerHost32: (instancePtr, context, fnIndex, funcSig, funcSigLength) => {
				let entry = getEntry(instancePtr);
				if (entry.hadInit) throw Error("Can't register host functions after .init()");

				let wasmFnIndex = entry.functionTable.length;
				entry.functionTable.grow(1);
				let sig = '';
				let sigBytes = new Uint8Array(this.hostMemory.buffer).subarray(funcSig, funcSig + funcSigLength);
				sigBytes.forEach(b => {
					sig += String.fromCharCode(b);
				});
				entry.hostFunctions['hostFn' + fnIndex] = {
					instanceIndex: wasmFnIndex,
					hostIndex: fnIndex,
					context: context,
					sig: sig,
					registrationIndex: Object.keys(entry.hostFunctions).length
				};
				return wasmFnIndex;
			},
			init32: instancePtr => {
				let entry = getEntry(instancePtr);
				if (entry.hadInit) throw Error("WCLAP initialised twice");
				entry.hadInit = true;
				
				this.#assignHostFunctions(entry);

				if (typeof entry.instance.exports._initialize === 'function') {
					entry.instance.exports._initialize();
				}

				return entry.instance.exports.clap_entry;
			},
			malloc32: (instancePtr, size) => {
				let entry = getEntry(instancePtr);
				let ptr = entry.instance.exports.malloc(size);
				return ptr;
			},
			countUntil32: (instancePtr, startPtr, untilValuePtr, size, maxCount) => {
				let entry = getEntry(instancePtr);
				let untilArray = new Uint8Array(this.hostMemory.buffer).subarray(untilValuePtr, untilValuePtr + size);
				let wclapA = new Uint8Array(entry.memory.buffer).subarray(startPtr);
				for (let i = 0; i < maxCount; ++i) {
					let offset = i*size;
					let difference = false;
					for (let b = 0; b < size; ++b) {
						if (wclapA[offset + b] != untilArray[b]) difference = true;
					}
					if (!difference) return i;
				}
				return maxCount;
			},
			call32: (instancePtr, wasmFn, isPtrToFn, resultPtr, argsPtr, argsCount) => {
				let entry = getEntry(instancePtr);
				if (!entry.hadInit) throw Error("WCLAP function called before initialisation");

				if (isPtrToFn) { // not a function index, but a pointer to one - used to avoid a round-trip just to get the function pointer
					let entryDataView = new DataView(entry.memory.buffer);
					wasmFn = entryDataView.getUint32(wasmFn, true);
				}

				let dataView = new DataView(this.hostMemory.buffer);
				let args = [];
				for (let i = 0; i < argsCount; ++i) {
					let ptr = argsPtr + i*16;
					let type = dataView.getUint8(ptr);
					if (type == 0) {
						args.push(dataView.getUint32(ptr + 8, true));
					} else if (type == 1) {
						args.push(dataView.getBigUint64(ptr + 8, true));
					} else if (type == 2) {
						args.push(dataView.getFloat32(ptr + 8, true));
					} else if (type == 3) {
						args.push(dataView.getFloat64(ptr + 8, true));
					} else {
						throw Error("invalid argument type");
					}
				}

				let result = entry.functionTable.get(wasmFn)(...args);
				if (result == null) {
					dataView.setUint8(resultPtr, 0);
					dataView.setUint32(resultPtr + 8, 0, true);
				} else if (typeof result == 'boolean') {
					dataView.setUint8(resultPtr, 0);
					dataView.setUint32(resultPtr + 8, result, true);
				} else if (typeof result == 'number' && (result|0) == result) {
					dataView.setUint8(resultPtr, 0);
					dataView.setInt32(resultPtr + 8, result, true);
				} else if (typeof result == 'number') {
					dataView.setUint8(resultPtr, 3);
					dataView.setFloat64(resultPtr + 8, result, true);
				} else if (result instanceof BigInt && result >= 0n) {
					dataView.setUint8(resultPtr, 1);
					dataView.setBigUint64(resultPtr + 8, result, true);
				} else if (result instanceof BigInt && result < 0n) {
					dataView.setUint8(resultPtr, 1);
					dataView.setBigInt64(resultPtr + 8, result, true);
				} else {
					console.error("Unknown return type from WCLAP function:", result);
				}
			},
			memcpyToOther32: (instancePtr, wclapP, hostP, size) => {
				let entry = getEntry(instancePtr);
				let wclapA = new Uint8Array(entry.memory.buffer).subarray(wclapP, wclapP + size);
				let hostA = new Uint8Array(this.hostMemory.buffer).subarray(hostP, hostP + size);
				wclapA.set(hostA);
				return true;
			},
			memcpyFromOther32: (instancePtr, hostP, wclapP, size) => {
				let entry = getEntry(instancePtr);
				let hostA = new Uint8Array(this.hostMemory.buffer).subarray(hostP, hostP + size);
				let wclapA = new Uint8Array(entry.memory.buffer).subarray(wclapP, wclapP + size);
				hostA.set(wclapA);
				return true;
			},
			// wclap64
			init64: instancePtr => {throw Error("64-bit WCLAP not supported (yet)")}
		};
		
		let wasiPromise = startWasi(config.wasi);

		this.ready = (async _ => {
			this.#config = config;
			let importMemory = config.memory;
			let needsInit = !importMemory;

			// Memory import (if defined)
			WebAssembly.Module.imports(config.module).forEach(entry => {
				if (entry.kind == 'memory') {
					if (!importMemory) {
						importMemory = new WebAssembly.Memory({initial: 2, maximum: 32768, shared: true});
						if (globalThis.crossOriginIsolated) config.memory = importMemory;
					}
					
					if (!hostImports[entry.module]) hostImports[entry.module] = {};
					hostImports[entry.module][entry.name] = importMemory;
				}
			});
			
			// Add WASI imports
			this.#wasi = await wasiPromise;
			config.wasi = this.#wasi.initObj();
			Object.assign(hostImports, this.#wasi.importObj);

			// wasi-threads
			if (!hostImports.wasi) hostImports.wasi = {};
			hostImports.wasi['thread-spawn'] = threadArg => {
				return this.#hostThreadSpawnWASIT1(threadArg);
			};

			this.hostInstance = await WebAssembly.instantiate(this.#config.module, hostImports);
			this.hostMemory = importMemory || this.hostInstance.exports.memory;
			
			for (let key in this.hostInstance.exports) {
				let e = this.hostInstance.exports[key];
				if (e instanceof WebAssembly.Table && typeof e.get(e.length - 1) == 'function') {
					this.#functionTable = e;
				}
			}

			this.#wasi.bindToOtherMemory(this.hostMemory);
			if (needsInit) this.hostInstance.exports._initialize();

			this.shared = !!config.memory;
			this.ready = true;
			return this;
		})();
	}
	
	initObj() {
		return Object.assign({}, this.#config);
	}
	
	/// Returns an instance pointer (`Instance *`) for the C++ host.
	async startWclap(wclapInitObj, createWorkerFn) {
		if (!wclapInitObj.module) throw Error('WCLAP init object must be from `getWclap()`');

		let wclapImports = {};

		let importMemory = wclapInitObj.memory;
		let needsInit = !('instancePtr' in wclapInitObj);
		let needsWasi = false;
		WebAssembly.Module.imports(wclapInitObj.module).forEach(entry => {
			if (/^wasi/.test(entry.module)) needsWasi = true;
			if (entry.kind == 'memory') {
				if (!importMemory) {
					if (globalThis.crossOriginIsolated) throw Error('imported memory not supplied');
					importMemory = new WebAssembly.Memory(wclapInitObj.memorySpec);
				}
				if (!wclapImports[entry.module]) wclapImports[entry.module] = {};
				wclapImports[entry.module][entry.name] = importMemory;
			}
		});

		let instancePtr = wclapInitObj.instancePtr;

		let pluginWasi = null;
		if (needsWasi) {
			pluginWasi = await this.#wasi.copyForRebinding();
			Object.assign(wclapImports, pluginWasi.importObj);

			if (needsInit && wclapInitObj.files) {
				pluginWasi.loadFiles(wclapInitObj.files);
				// TODO: delete these files, since they'll now get passed around inside the WASI's memory
			}
		}
		// wasi-threads
		if (!wclapImports.wasi) wclapImports.wasi = {};
		wclapImports.wasi['thread-spawn'] = threadArg => {
			return this.#pluginThreadSpawnWASIT1(instancePtr, threadArg);
		};

		let pluginInstance = await WebAssembly.instantiate(wclapInitObj.module, wclapImports);
		let functionTable = null;
		for (let name in pluginInstance.exports) {
			if (pluginInstance.exports[name] instanceof WebAssembly.Table) {
				let table = pluginInstance.exports[name];
				if (table.length > 0 && typeof table.get(table.length - 1) === 'function') {
					if (functionTable) throw Error("WCLAP exported multiple function tables");
					functionTable = table;
				}
			}
		}
		if (!functionTable) throw Error("WCLAP didn't export a function table");

		let is64 = pluginInstance.exports.clap_entry instanceof BigInt;
		let entry = {
			is64: is64,
			initObj: wclapInitObj,
			instance: pluginInstance,
			memory: importMemory || pluginInstance.exports.memory,
			functionTable: functionTable,
			hostFunctions: wclapInitObj.hostFunctions || {},
			createWorkerFn: createWorkerFn
		};
		if (pluginWasi) {
			pluginWasi.bindToOtherMemory(entry.memory);
		}

		if (needsInit) {
			if (is64) throw Error("wasm64 WCLAP isn't supported yet");
			instancePtr = this.hostInstance.exports._wclapInstanceCreate(is64);
			if (!instancePtr) throw Error("creating WCLAP `Instance *` failed");
			// Set the path
			let pathBytes = new TextEncoder('utf-8').encode(wclapInitObj.pluginPath);
			let pathPtr = this.hostInstance.exports._wclapInstanceSetPath(instancePtr, pathBytes.length);
			new Uint8Array(this.hostMemory.buffer).set(pathBytes, pathPtr);
		} else {
			if (!wclapInitObj.hostFunctions) throw Error("Starting WCLAP thread, but host functions not provided - did you use `host.getWorkerData()`?");
			this.#assignHostFunctions(entry);
			entry.hadInit = true;
		}
		let shared = !!wclapInitObj.memory;
		this.#wclapMap[instancePtr] = entry;

		return {
			ptr: instancePtr,
			memory: entry.memory,
			shared: shared
		};
	}
	
	#assignHostFunctions(entry) {
		let makeHostFunctionsNative = hostFnEntries => {
			let fnSigs = {};
			let imports = {proxy:{}};
			for (let key in hostFnEntries) {
				let hostFnEntry = hostFnEntries[key];
				fnSigs[key] = hostFnEntry.sig;
				let hostFn = this.#functionTable && this.#functionTable.get(hostFnEntry.hostIndex);
				if (typeof hostFn !== 'function') {
					let exportName = this.#hostCallbackOrder[hostFnEntry.registrationIndex];
					hostFn = exportName ? this.hostInstance.exports[exportName] : null;
				}
				if (typeof hostFn !== 'function') throw Error("Host callback lookup failed");
				imports.proxy[key] = hostFn.bind(null, hostFnEntry.context);
			}
			let wasm = generateForwardingWasm(fnSigs);
			// Synchronous compile & instantiate
			let forwardingModule = new WebAssembly.Module(wasm);
			let forwardingInstance = new WebAssembly.Instance(forwardingModule, imports);
			return forwardingInstance.exports;
		};
		let nativeHostFns = makeHostFunctionsNative(entry.hostFunctions);
		for (let key in entry.hostFunctions) {
			let fnEntry = entry.hostFunctions[key];
			if (entry.functionTable.length <= fnEntry.instanceIndex) {
				entry.functionTable.grow(fnEntry.instanceIndex + 1 - entry.functionTable.length);
			}
			entry.functionTable.set(fnEntry.instanceIndex, nativeHostFns[key]);
		}
	}
}

async function startHost(initObj, hostImports) {
	initObj = Object.assign({}, initObj);
	return new WclapHost(initObj, hostImports).ready;
}

async function getHost(initObj) {
	if (typeof initObj == 'string') initObj = {url: initObj};
	if (initObj.module) return initObj;

	if (!initObj.module) {
		initObj.url = new URL(initObj.url, document.baseURI).href;
		initObj.module = await WebAssembly.compileStreaming(fetch(initObj.url));
	}
	if (!initObj.wasi) initObj.wasi = await getWasi();
	return initObj;
}

// Worklets don't have TextEncoder/TextDecoder - this polyfill isn't the most performant, but we shouldn't be doing it often in an AudioProcessorWorklet anyway
if (!globalThis.TextEncoder) {
	let TextCodec = globalThis.TextEncoder = globalThis.TextDecoder = function(){};
	TextCodec.prototype.encode = str => {
		let binaryString = unescape(encodeURIComponent(str));
		let result = new Uint8Array(binaryString.length);
		for (let i = 0; i < binaryString.length; ++i) {
			result[i] = binaryString.charCodeAt(i);
		}
		return result;
	};
	TextCodec.prototype.decode = array => {
		if (!(array instanceof ArrayBuffer) && !ArrayBuffer.isView(array)) {
			throw Error('Can only use ArrayBuffer or view with TextDecoder');
		}
		array = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
		let binaryString = "";
		for (let i = 0; i < array.length; ++i) {
			binaryString += String.fromCharCode(array[i]);
		}
		return decodeURIComponent(escape(binaryString));
	};
}

// You need to call this from any new Workers you create from
async function runThread(threadData, hostImports, createWorkerFn) {
	console.log("Running WCLAP Worker thread");
	return WclapHost.runWorker(threadData, hostImports, createWorkerFn);
}
