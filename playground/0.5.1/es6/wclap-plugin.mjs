import expandTarGz from "./targz.mjs"

function fnv1aHex(string) {
	let fnv1a32 = 0x811c9dc5;
	for (let i = 0; i < string.length; ++i) {
		let byte = string.charCodeAt(i);
		fnv1a32 = ((fnv1a32^byte)*0x1000193)|0;
	}
	return [24, 16, 8, 0].map(s => ((fnv1a32>>s)&0xFF).toString(16).padStart(2, "0")).join("");
}

export default async function getWclap(options) {
	if (typeof options === 'string') options = {url: options};
	options = Object.assign({}, options);
	if (!options.pluginPath) options.pluginPath = "/plugin/" + fnv1aHex(options.url);
	if (options.module && options.module instanceof WebAssembly.Module) {
		// Make a distinct copy of the memory (if it exists)
		if (options.memory) options.memory = new WebAssembly.Memory(options.memorySpec);
		// Distinct path suffix
		options.pluginPath += "-copy-" + fnv1aHex(Date.now() + options.url + Math.random());
		return options;
	}

	let prevFiles = options.files;
	options.files = {};
	if (prevFiles) {
		for (let key in prevFiles) { // Add the WCLAP's path prefix
			options.files[`${options.pluginPath}/${key}`] = prevFiles[key];
		}
	}

	function guessMemorySize(bufferOrSize, module) {
		let importsMemory = false;
		WebAssembly.Module.imports(module).forEach(entry => {
			if (entry.kind == 'memory') importsMemory = true;
		});
		if (!importsMemory) return;
	
		// We have to guess the imported memory size - as a heuristic, use the module size itself
		if (ArrayBuffer.isView(bufferOrSize)) bufferOrSize = bufferOrSize.buffer;
		let moduleSize = (typeof bufferOrSize == 'number' ? bufferOrSize : bufferOrSize.byteLength);
		let modulePages = Math.max(Math.ceil(moduleSize/65536) || 4, 4);
		options.memorySpec = {initial: modulePages, maximum: 32768, shared: true};
		// If we're cross-origin isolated, actually create this memory
		if (globalThis.crossOriginIsolated) options.memory = new WebAssembly.Memory(options.memorySpec);
	}

	let wasmPath = `${options.pluginPath}/module.wasm`;
	options.module = options.module || options.files[wasmPath];
	options.files[wasmPath] = new ArrayBuffer(0); // avoid self-parsing shenanigans

	if (options.module && (options.module instanceof ArrayBuffer || ArrayBuffer.isView(options.module))) {
		let buffer = options.module;
		options.module = await WebAssembly.compile(buffer);
		guessMemorySize(buffer, module);
		return options;
	}

	let response = await fetch(options.url);
	if (response.headers.get("Content-Type") == "application/wasm") {
		options.module = await WebAssembly.compileStreaming(response);
		guessMemorySize(response.headers.get('Content-Length') || (1<<24), options.module);
		return options;
	}

	// If it's not WASM, assume it's a `.tar.gz`
	let tarFiles = await expandTarGz(response);
	for (let path in tarFiles) {
		options.files[`${options.pluginPath}/${path}`] = tarFiles[path];
	}
	if (!options.files[wasmPath]) {
		// Find first `module.wasm` in the bundle (in case it's not top-level)
		for (let path in tarFiles) {
			if (/\/module.wasm$/.test(key)) {
				console.error(`WCLAP bundle has WASM at ${path} instead of /module.wasm`);
				wasmPath = `${options.pluginPath}/${path}`;
				break;
			}
		}
	}
	if (!options.files[wasmPath]) {
		throw Error("No `module.wasm` found in WCLAP bundle");
	}

	options.module = await WebAssembly.compile(options.files[wasmPath]);
	guessMemorySize(options.files[wasmPath], options.module);

	return options;
}
