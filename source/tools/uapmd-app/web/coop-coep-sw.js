/**
 * coop-coep-sw.js — Service Worker that injects Cross-Origin-Opener-Policy and
 * Cross-Origin-Embedder-Policy headers into every response.
 *
 * These two headers together enable the "cross-origin isolated" context that
 * browsers require before they expose SharedArrayBuffer.  SharedArrayBuffer is
 * in turn required by the Emscripten WASM Workers / AudioWorklet build
 * (-sSHARED_MEMORY=1 -sWASM_WORKERS=1 -sAUDIO_WORKLET=1).
 *
 * Usage: load index.html once (without the headers), the SW registers itself,
 * reloads the page, and from the second load onward every fetch response
 * carries the necessary headers automatically.
 *
 * No server-side changes are required — this works with any static file host
 * including `npx serve`, GitHub Pages, or a plain `python -m http.server`.
 *
 * Note: for production deployments it is preferable to set these headers
 * server-side (e.g. in nginx/Apache config) instead.
 */

'use strict';

const CACHE_NAME = 'uapmd-coop-coep-v1';

// ── Install: skip waiting so the SW activates immediately ───────────────────
self.addEventListener('install', () => self.skipWaiting());

// ── Activate: take control of all open clients right away ──────────────────
self.addEventListener('activate', (event) => {
    event.waitUntil(self.clients.claim());
});

// ── Fetch: re-attach COOP/COEP headers to every response ──────────────────
self.addEventListener('fetch', (event) => {
    // Skip opaque/cross-origin-only-if-cached requests that would throw.
    if (event.request.cache === 'only-if-cached' &&
        event.request.mode  !== 'same-origin') {
        return;
    }

    // For the WASM binary and main JS bundle, bypass the HTTP cache so a
    // newly-built file is always picked up without a hard refresh.
    const url = event.request.url;
    const isBuildArtifact = /\.(wasm|js)$/.test(url) &&
                            !url.includes('coop-coep-sw');
    const fetchRequest = isBuildArtifact
        ? new Request(event.request, { cache: 'no-store' })
        : event.request;

    event.respondWith(
        fetch(fetchRequest).then((response) => {
            // Clone headers so we can add to them.
            const headers = new Headers(response.headers);
            headers.set('Cross-Origin-Opener-Policy',   'same-origin');
            headers.set('Cross-Origin-Embedder-Policy', 'require-corp');

            return new Response(response.body, {
                status:     response.status,
                statusText: response.statusText,
                headers,
            });
        })
    );
});
