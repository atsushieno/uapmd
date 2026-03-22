// Service Worker: inject COOP/COEP headers so SharedArrayBuffer is available.
// Required for AudioWorklet + SharedArrayBuffer (WASM_WORKERS=1, SHARED_MEMORY=1).
//
// Registration: index.html registers this SW before loading the WASM module.
// On reload the SW intercepts all fetch responses and adds:
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
    const url = event.request.url;

    // Force network fetch (bypass HTTP cache) for WASM and JS bundles so that a
    // hard reload always picks up the latest build rather than a stale SW-cached copy.
    const noCache = /\.(wasm|js)(\?.*)?$/.test(url);
    const fetchRequest = noCache
        ? new Request(event.request, { cache: 'no-cache' })
        : event.request;

    event.respondWith(
        fetch(fetchRequest).then((response) => {
            // Only mutate same-origin responses to avoid CORS issues with CDN assets.
            if (!response.url.startsWith(self.location.origin))
                return response;

            const headers = new Headers(response.headers);
            headers.set('Cross-Origin-Opener-Policy', 'same-origin');
            headers.set('Cross-Origin-Embedder-Policy', 'require-corp');

            return new Response(response.body, {
                status: response.status,
                statusText: response.statusText,
                headers,
            });
        })
    );
});
