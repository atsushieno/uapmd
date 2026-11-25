// Service Worker: inject COOP/COEP headers so SharedArrayBuffer is available.
// Required for AudioWorklet + SharedArrayBuffer (WASM_WORKERS=1, SHARED_MEMORY=1).
//
// Registration: index.html registers this SW before loading the WASM module.
// On reload the SW intercepts all fetch responses and adds:
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

function withIsolationHeaders(response) {
    if (!response || !response.url.startsWith(self.location.origin))
        return response;

    // 204, 205 and 304 are null-body statuses, and `new Response(body, {status})`
    // throws a TypeError for them. The throw happens inside the promise passed to
    // respondWith(), so the browser blames the page being fetched: "A ServiceWorker
    // intercepted the request and encountered an unexpected error". These responses
    // carry no body to re-wrap and no document to isolate, so pass them straight
    // through. This matters most on static hosts such as GitHub Pages, which answer
    // conditional requests with 304 on every revisit.
    if (response.status === 204 || response.status === 205 || response.status === 304)
        return response;

    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');

    return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers,
    });
}

const pendingAppScripts = new Map();

async function fetchAppScript(request) {
    const key = JSON.stringify([
        request.url, request.mode, request.credentials, request.redirect,
        request.integrity, request.referrer, request.referrerPolicy,
        Array.from(request.headers),
    ]);
    let pending = pendingAppScripts.get(key);
    if (!pending) {
        pending = (async () => {
            const response = await fetch(request);
            if (!response.ok || !response.body
                    || new URL(response.url).origin !== self.location.origin)
                return withIsolationHeaders(response);

            // fetch() resolves at headers, before the HTTP cache reader finishes.
            // Keep identical pthread bundle requests together until the whole body
            // has been read: overlapping revalidations reproduce a truncated read
            // in Firefox. Each worker then gets its own memory-backed response.
            const isolated = withIsolationHeaders(response);
            const body = await isolated.arrayBuffer();
            return new Response(body, {
                status: isolated.status,
                statusText: isolated.statusText,
                headers: isolated.headers,
            });
        })().finally(() => pendingAppScripts.delete(key));
        pendingAppScripts.set(key, pending);
    }
    return (await pending).clone();
}

self.addEventListener('fetch', (event) => {
    const requestUrl = new URL(event.request.url);

    // Firefox can reject AudioWorklet module fetches when they are fulfilled
    // through a service worker. The worklet script is same-origin, so it does
    // not need COEP header injection here.
    if (event.request.destination === 'audioworklet'
            || requestUrl.pathname.endsWith('/uapmd-webclap-worklet.js'))
        return;

    const url = event.request.url;

    // Revalidate WASM and JS bundles so reloads pick up the latest build.
    const noCache = /\.(wasm|m?js)(\?.*)?$/.test(url);
    const fetchRequest = noCache
        ? new Request(event.request, { cache: 'no-cache' })
        : event.request;

    // Emscripten loads this bundle again for every pthread in the startup pool.
    if (event.request.method === 'GET'
            && requestUrl.origin === self.location.origin
            && requestUrl.pathname === new URL('uapmd-app.js', self.registration.scope).pathname) {
        event.respondWith(fetchAppScript(fetchRequest));
        return;
    }

    event.respondWith(
        fetch(fetchRequest).then((response) => {
            return withIsolationHeaders(response);
        })
    );
});
