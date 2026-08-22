// AudioWorklet environment shim for Emscripten WASM workers.
//
// Problem 1 — AudioWorkletGlobalScope detection:
//   Emscripten detects the AudioWorklet context via !!globalThis.AudioWorkletGlobalScope.
//   Chrome does not expose AudioWorkletGlobalScope as a named property on the scope
//   itself, so that check evaluates to false inside AudioWorkletGlobalScope, causing
//   registerProcessor("em-bootstrap") to be skipped and AudioWorkletNode creation to
//   throw NotSupportedError every time the audio engine starts.
//
//   Fix: registerProcessor is only defined inside AudioWorkletGlobalScope, so we
//   use its presence as a reliable sentinel and backfill the missing named property.
//
// Problem 2 — navigator not defined:
//   AudioWorkletGlobalScope does not expose window/worker navigator APIs.  Emscripten
//   assumes navigator exists (userAgent sniffing, hardwareConcurrency, Web MIDI stubs),
//   so a structured shim is required to avoid ReferenceError crashes when the same
//   runtime executes inside the worklet.
//
// Problem 3 — WebGL uniform uploads from shared/resizable WASM memory:
//   With pthreads + memory growth, Emscripten's WebGL2 glue can pass HEAPF32/HEAP32
//   directly to uniform*() using the WebGL2 srcOffset/srcLength overloads. Newer
//   Chromium rejects typed arrays backed by SharedArrayBuffer or resizable
//   ArrayBuffer for these calls. Copy just those upload arguments to temporary
//   fixed ArrayBuffers; texture/buffer uploads stay on Emscripten's fast path.

(() => {
    const isUnsafeUniformView = (value) => {
        if (!value || !ArrayBuffer.isView(value) || !value.buffer)
            return false;
        if (typeof SharedArrayBuffer !== 'undefined' && value.buffer instanceof SharedArrayBuffer)
            return true;
        return value.buffer.resizable === true || value.buffer.growable === true;
    };

    const copyUniformView = (value, offset, length) => {
        if (typeof offset === 'number' && typeof length === 'number')
            return new value.constructor(value.subarray(offset, offset + length));
        if (typeof offset === 'number')
            return new value.constructor(value.subarray(offset));
        return new value.constructor(value);
    };

    const wrapUniformUpload = (proto, name, dataArgIndex) => {
        if (!proto || typeof proto[name] !== 'function')
            return;
        const original = proto[name];
        proto[name] = function(...args) {
            const view = args[dataArgIndex];
            if (!isUnsafeUniformView(view))
                return original.apply(this, args);

            const fixedArgs = args.slice(0, dataArgIndex);
            fixedArgs.push(copyUniformView(view, args[dataArgIndex + 1], args[dataArgIndex + 2]));
            return original.apply(this, fixedArgs);
        };
    };

    const uniformVectorMethods = [
        'uniform1fv', 'uniform2fv', 'uniform3fv', 'uniform4fv',
        'uniform1iv', 'uniform2iv', 'uniform3iv', 'uniform4iv',
        'uniform1uiv', 'uniform2uiv', 'uniform3uiv', 'uniform4uiv',
    ];
    const uniformMatrixMethods = [
        'uniformMatrix2fv', 'uniformMatrix3fv', 'uniformMatrix4fv',
        'uniformMatrix2x3fv', 'uniformMatrix2x4fv',
        'uniformMatrix3x2fv', 'uniformMatrix3x4fv',
        'uniformMatrix4x2fv', 'uniformMatrix4x3fv',
    ];

    const install = (ctor) => {
        if (!ctor || !ctor.prototype)
            return;
        for (const name of uniformVectorMethods)
            wrapUniformUpload(ctor.prototype, name, 1);
        for (const name of uniformMatrixMethods)
            wrapUniformUpload(ctor.prototype, name, 2);
    };

    install(globalThis.WebGLRenderingContext);
    install(globalThis.WebGL2RenderingContext);
})();

if (typeof registerProcessor === 'function') {
    globalThis.AudioWorkletGlobalScope = globalThis;

    if (typeof navigator === 'undefined') {
        const defineReadOnly = (target, key, value) => {
            Object.defineProperty(target, key, {
                configurable: false,
                enumerable: true,
                writable: false,
                value,
            });
        };

        const immutableUserActivation = Object.freeze({
            hasBeenActive: false,
            isActive: false,
        });

        const unavailableError = (api) => {
            const err = new Error(`${api} is not available inside AudioWorkletGlobalScope`);
            err.name = 'NotSupportedError';
            return err;
        };

        const rejectUnavailable = (api) => () => Promise.reject(unavailableError(api));

        const mediaDevicesShim = Object.freeze({
            enumerateDevices: () => Promise.resolve([]),
            getUserMedia: rejectUnavailable('mediaDevices.getUserMedia'),
        });

        const baseNavigator = Object.create(null);
        defineReadOnly(baseNavigator, 'hardwareConcurrency', 1);
        defineReadOnly(baseNavigator, 'language', 'en-US');
        defineReadOnly(baseNavigator, 'languages', Object.freeze(['en-US']));
        defineReadOnly(baseNavigator, 'platform', 'AudioWorklet');
        defineReadOnly(
            baseNavigator,
            'userAgent',
            'Mozilla/5.0 (AudioWorklet) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36'
        );
        defineReadOnly(baseNavigator, 'vendor', 'uapmd');
        defineReadOnly(baseNavigator, 'webdriver', false);
        defineReadOnly(baseNavigator, 'userActivation', immutableUserActivation);
        defineReadOnly(baseNavigator, 'getGamepads', () => []);
        defineReadOnly(baseNavigator, 'mediaDevices', mediaDevicesShim);
        defineReadOnly(baseNavigator, 'permissions', undefined);
        defineReadOnly(baseNavigator, 'serviceWorker', undefined);
        defineReadOnly(baseNavigator, 'requestMIDIAccess', rejectUnavailable('navigator.requestMIDIAccess'));

        const navigatorProxy = new Proxy(baseNavigator, {
            get(target, prop) {
                if (prop === Symbol.toStringTag) return 'Navigator';
                return target[prop];
            },
            has(target, prop) {
                return Object.prototype.hasOwnProperty.call(target, prop);
            },
        });

        Object.defineProperty(globalThis, 'navigator', {
            configurable: false,
            enumerable: false,
            writable: false,
            value: navigatorProxy,
        });
    }
}
