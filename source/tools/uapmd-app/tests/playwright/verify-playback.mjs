/**
 * Playwright verification for uapmd WASM playback.
 *
 * Usage:
 *   UAPMD_WASM_WEB_ROOT=/abs/path/to/uapmd-app node verify-playback.mjs
 *   node verify-playback.mjs /abs/path/to/uapmd-app
 *
 * Checks:
 *  1. App loads without abort
 *  2. crossOriginIsolated == true
 *  3. Audio engine can be enabled via toolbar button
 *  4. Play button advances the playhead position
 */
import http from 'http';
import fs from 'fs';
import path from 'path';
import { chromium } from 'playwright';

function resolveServeRoot() {
    const argRoot = process.argv[2];
    const envRoot = process.env.UAPMD_WASM_WEB_ROOT;
    const root = argRoot || envRoot;
    if (!root) {
        throw new Error('Missing wasm web root. Pass it as argv[2] or set UAPMD_WASM_WEB_ROOT.');
    }
    return path.resolve(root);
}

const SERVE_ROOT = resolveServeRoot();
const PORT = 18766;
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

function getLatestPos(msgs) {
    const posLines = msgs.filter(m => m.text.includes('[uapmd-pos]'));
    if (!posLines.length) return null;
    const last = posLines[posLines.length - 1].text;
    const m = last.match(/\[uapmd-pos\]\s+(-?\d+)\s+isPlaying=(\d)\s+engineEnabled=(\d)/);
    if (!m) return null;
    return { samples: parseInt(m[1]), isPlaying: m[2] === '1', engineEnabled: m[3] === '1' };
}

const browser = await chromium.launch({
    headless: false,
    channel: 'chrome',
    args: [
        '--no-sandbox',
        '--disable-dev-shm-usage',
        '--enable-webgl',
        '--use-gl=angle',
        '--use-angle=swiftshader',
        '--ignore-gpu-blocklist',
        '--autoplay-policy=no-user-gesture-required',
    ],
});
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });

const consoleMsgs = [];
const pageErrors  = [];
page.on('console', m => consoleMsgs.push({ type: m.type(), text: m.text() }));
page.on('pageerror', e => pageErrors.push(e.message));

console.log('Loading page…');
await page.goto(`http://localhost:${PORT}/`, { waitUntil: 'domcontentloaded', timeout: 15000 });

await (async () => {
    const deadline = Date.now() + 45000;
    while (Date.now() < deadline) {
        if (consoleMsgs.some(m => m.text.includes('[uapmd-pos]'))) break;
        await page.waitForTimeout(500);
    }
})();

const hasAbortAtStart = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));
const isolated = await page.evaluate(() => window.crossOriginIsolated);
console.log(`crossOriginIsolated: ${isolated}`);
console.log(`Abort at startup: ${hasAbortAtStart}`);

const posBeforeEngine = getLatestPos(consoleMsgs);
console.log(`Position before engine: ${JSON.stringify(posBeforeEngine)}`);

await page.mouse.click(210, 25);
await page.waitForTimeout(500);

await (async () => {
    const deadline = Date.now() + 8000;
    while (Date.now() < deadline) {
        const pos = getLatestPos(consoleMsgs);
        if (pos && pos.engineEnabled) break;
        await page.waitForTimeout(500);
    }
})();

await page.mouse.click(462, 25);
await page.waitForTimeout(1000);

await (async () => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
        const pos = getLatestPos(consoleMsgs);
        if (pos && pos.isPlaying) break;
        await page.waitForTimeout(500);
    }
})();

const pos0 = getLatestPos(consoleMsgs);
await page.waitForTimeout(5000);
const pos1 = getLatestPos(consoleMsgs);

const playheadAdvanced = pos1 && pos0 && pos1.samples > pos0.samples;
const engineOnAny = consoleMsgs.some(m => m.text.includes('engineEnabled=1'));
const wasPlaying = consoleMsgs.some(m => m.text.includes('isPlaying=1'));
const hasAbortAny = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));

console.log(`Playhead advanced: ${playheadAdvanced}  (${pos0?.samples} → ${pos1?.samples})`);
console.log(`Engine was enabled at some point: ${engineOnAny}`);
console.log(`Was playing at some point: ${wasPlaying}`);
console.log(`Any abort: ${hasAbortAny}`);

if (pageErrors.length) {
    console.log('\n=== Page errors ===');
    pageErrors.forEach(e => console.log(`  ${e}`));
}

const pass = isolated && !hasAbortAtStart && wasPlaying && playheadAdvanced;
console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`);

await browser.close();
server.close();
process.exit(pass ? 0 : 1);
