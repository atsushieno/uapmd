# `wasi.wasm`: WASI functions implemented as a WASM module

The goal is to provide a WASM implementation with stdout/stderr, and an in-memory VFS.

## WASM module

The module imports its (shared) memory, and some functions which help it manipulate the memory of the (soon to be instantiated) module it's providing WASI imports for.

Multiple modules can use the same WASI setup (with a shared VFS etc.) by creating new instances which use the same shared memory but different imports.

The `wasi.wasm` module *also* has a couple of WASI-P1 imports itself - but it shouldn't ever actually call these, so you can just provide stubs that do nothing (or throw).

## JS loader

There's an ES6 module `wasi.mjs` (or `wasi-bundled.mjs` which includes the WASM inline), with the exports `getWasi()` (which compiles the module and `Promise`-resolves to an init object), and `startWasi(optionalInitObj)`.

The Wasi object obtained with `startWasi()` provides a `.importObj` property, which can be used to supply the imports for one other module.  You must bind to that module's memory using `bindToOtherMemory()` when it's available.

```js
let wasi = await startWasi();
Object.assign(otherModuleImports, wasi.importObj);

// when available
wasi.bindToOtherMemory(otherModuleMemory);
```

### Shared WASI

To share the VFS/etc. with a new WASI-based module in the same JS context, you can create copies with `wasi.copyForRebinding()`:

```js
let wasi2 = await wasi.copyForRebinding();
Object.assign(module2Imports, wasi2.importObj);

wasi2.bindToOtherMemory(module2Memory);
// Both modules will now share the same WASI VFS
```

To share the same WASI setup on another Worker/Worklet, use `.initObj()` and then pass that to `.startWasi()` in the other context:

```js
// on existing thread
let wasiInit = wasi.initObj();
startMyWorker({wasiInit, ...});

// in new Worker/Worklet
let wasi = await startWasi(wasiInit);
```

The actual WASI data (e.g. VFS contents) will only be shared if the page is cross-origin isolated.  Otherwise, `.initObj()` only includes the precompiled module, but doesn't try to pass the shared memory across.

## Development

The C++ code is in `dev/`.  Assuming `WASI_SDK` points to a [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) release:

```sh
cmake . -B cmake-build -DCMAKE_TOOLCHAIN_FILE=$(WASI_SDK)/share/cmake/wasi-sdk-pthread.cmake  -DCMAKE_BUILD_TYPE=Release
# outputs ../wasi.wasm
cmake --build cmake-build --target wasi --config Release
```

To update the bundled version, run `node make-bundled.js`.
