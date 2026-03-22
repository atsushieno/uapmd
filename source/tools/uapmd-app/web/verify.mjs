/**
 * Playwright verification for uapmd WASM build.
 * Run from this directory: node verify.mjs
 *
 * Checks:
 *  1. App loads without abort
 *  2. crossOriginIsolated == true (SharedArrayBuffer available)
 *  3. Audio engine starts as Off
 *  4. Clicking audio engine button does not produce abort
 */
import http from 'http';
import fs from 'fs';
import path from 'path';
import { chromium } from 'playwright';

// ── Server ──────────────────────────────────────────────────────────────────
const SERVE_ROOT = path.resolve('../../../../cmake-build-wasm/source/tools/uapmd-app');
const PORT = 18765;
const MIME = { '.html':'text/html', '.js':'application/javascript',
               '.wasm':'application/wasm', '.css':'text/css' };

const server = http.createServer((req, res) => {
    const filePath = path.join(SERVE_ROOT, req.url === '/' ? '/index.html' : req.url);
    fs.readFile(filePath, (err, data) => {
        if (err) { res.writeHead(404); res.end('not found'); return; }
        res.writeHead(200, {
            'Content-Type': MIME[path.extname(filePath)] || 'application/octet-stream',
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
            'Cross-Origin-Resource-Policy': 'same-origin',
        });
        res.end(data);
    });
});
await new Promise(r => server.listen(PORT, r));
console.log(`Server: http://localhost:${PORT}  (root: ${SERVE_ROOT})`);

// ── Browser ─────────────────────────────────────────────────────────────────
const browser = await chromium.launch({
    headless: true,
    channel: 'chrome',  // use system Chrome for better GPU support
    args: [
        '--headless=new',
        '--enable-webgl', '--use-gl=swiftshader', '--ignore-gpu-blocklist',
        '--enable-gpu-rasterization',
        '--autoplay-policy=no-user-gesture-required',
    ],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

const consoleMsgs = [];
const pageErrors  = [];
page.on('console', m => consoleMsgs.push({ type: m.type(), text: m.text() }));
page.on('pageerror', e => pageErrors.push(e.message));

// ── Load ────────────────────────────────────────────────────────────────────
console.log('Loading page…');
await page.goto(`http://localhost:${PORT}/`, { waitUntil: 'domcontentloaded', timeout: 15000 });

// Wait for WASM init: poll Node-side consoleMsgs for the first uapmd log
await (async () => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
        if (consoleMsgs.some(m => m.text.includes('uapmd'))) break;
        await page.waitForTimeout(500);
    }
})();

await page.waitForTimeout(5000); // extra settling time

// ── Startup screenshot ───────────────────────────────────────────────────────
await page.screenshot({ path: '/tmp/uapmd-01-startup.png', fullPage: true });
console.log('Screenshot: /tmp/uapmd-01-startup.png');

// ── Check crossOriginIsolated ────────────────────────────────────────────────
const isolated = await page.evaluate(() => window.crossOriginIsolated);
console.log(`crossOriginIsolated: ${isolated}`);

// ── Look for abort / bad_alloc before any interaction ───────────────────────
const hasAbortAtStart = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));
const hasEngineOnAtStart = consoleMsgs.some(m => /Audio Engine.*On/i.test(m.text));
console.log(`Abort at startup: ${hasAbortAtStart}`);
console.log(`Engine auto-On at startup: ${hasEngineOnAtStart}`);

// ── Simulate user clicking the audio engine button ───────────────────────────
// ImGui renders the toolbar near the top; try the left area of the toolbar.
// Click several candidate x positions at y=25 to hit the button.
for (const x of [100, 150, 200, 250, 300]) {
    await page.mouse.click(x, 25);
    await page.waitForTimeout(300);
}
await page.waitForTimeout(4000);
await page.screenshot({ path: '/tmp/uapmd-02-after-click.png', fullPage: true });
console.log('Screenshot: /tmp/uapmd-02-after-click.png');

// ── Check for abort after click ──────────────────────────────────────────────
const hasAbortAfterClick = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));
const hasEngineOnAfterClick = consoleMsgs.some(m => /Audio Engine.*On/i.test(m.text));
console.log(`Abort after click: ${hasAbortAfterClick}`);
console.log(`Engine turned On: ${hasEngineOnAfterClick}`);

// ── Full log ─────────────────────────────────────────────────────────────────
console.log('\n=== All console messages ===');
for (const m of consoleMsgs) console.log(`  [${m.type}] ${m.text}`);

console.log('\n=== Page errors ===');
pageErrors.length ? pageErrors.forEach(e => console.log(`  ${e}`)) : console.log('  (none)');

// ── Result ───────────────────────────────────────────────────────────────────
const pass = isolated && !hasAbortAtStart && !hasAbortAfterClick;
console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`);

await browser.close();
server.close();
process.exit(pass ? 0 : 1);
