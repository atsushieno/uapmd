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
