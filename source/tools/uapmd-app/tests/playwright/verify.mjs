/**
 * Playwright verification for uapmd WASM build.
 *
 * Usage:
 *   UAPMD_WASM_WEB_ROOT=/abs/path/to/uapmd-app node verify.mjs
 *   node verify.mjs /abs/path/to/uapmd-app
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

const browser = await chromium.launch({
    headless: true,
    channel: 'chrome',
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

console.log('Loading page…');
await page.goto(`http://localhost:${PORT}/`, { waitUntil: 'domcontentloaded', timeout: 15000 });

await (async () => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
        if (consoleMsgs.some(m => m.text.includes('uapmd'))) break;
        await page.waitForTimeout(500);
    }
})();

await page.waitForTimeout(5000);

const isolated = await page.evaluate(() => window.crossOriginIsolated);
const hasAbortAtStart = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));

for (const x of [100, 150, 200, 250, 300]) {
    await page.mouse.click(x, 25);
    await page.waitForTimeout(300);
}
await page.waitForTimeout(4000);

const hasAbortAfterClick = consoleMsgs.some(m => /abort|bad_alloc/i.test(m.text));
const pass = isolated && !hasAbortAtStart && !hasAbortAfterClick;

console.log(`crossOriginIsolated: ${isolated}`);
console.log(`Abort at startup: ${hasAbortAtStart}`);
console.log(`Abort after click: ${hasAbortAfterClick}`);

if (pageErrors.length) {
    console.log('\n=== Page errors ===');
    pageErrors.forEach(e => console.log(`  ${e}`));
}

console.log(`\n=== RESULT: ${pass ? 'PASS' : 'FAIL'} ===`);

await browser.close();
server.close();
process.exit(pass ? 0 : 1);
