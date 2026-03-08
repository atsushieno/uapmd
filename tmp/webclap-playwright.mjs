import playwrightModule from '../source/tools/uapmd-app/web/node_modules/playwright/index.mjs';

const { firefox } = playwrightModule;

async function run() {
  const targetUrl = process.env.UAPMD_WEB_URL ?? 'http://localhost:8080';
  const browser = await firefox.launch({ headless: true });
  const page = await browser.newPage();

  page.on('console', (msg) => {
    const location = msg.location();
    const locText = location?.url ? `${location.url}:${location.lineNumber ?? 0}` : '';
    const text = msg.text();
    if (!text.includes('[WebCLAP]') && !text.includes('[wasm-debug]')) {
      return;
    }
    console.log(`[browser:${msg.type()}] ${text} ${locText}`.trim());
  });
  page.on('pageerror', (err) => {
    console.error('[browser:pageerror]', err);
  });

  await page.goto(targetUrl, { waitUntil: 'domcontentloaded' });

  await page.waitForFunction(
    () => typeof Module !== 'undefined' && typeof Module.__uapmdUnlockAudio === 'function',
    null,
    { timeout: 60000 }
  );
  await page.evaluate(() => {
    Module.__uapmdUnlockAudio();
  });

  await page.evaluate(() => {
    globalThis.__uapmdWebClapTraceProcess = true;
    if (globalThis.uapmdWebClap) {
      globalThis.uapmdWebClap.traceProcess = true;
      globalThis.uapmdWebClap.traceProcessCount = 0;
    }
  });

  await page.waitForFunction(
    () => typeof Module !== 'undefined' && typeof Module.ccall === 'function',
    null,
    { timeout: 60000 }
  );

  await page.waitForFunction(
    () => typeof Module !== 'undefined' && Module.calledRun === true,
    null,
    { timeout: 60000 }
  );

  await page.evaluate(() => {
    Module.ccall('uapmd_debug_spawn_webclap_instance', 'number', [], []);
  });

  // Keep the page running briefly to capture console logs originating from the instantiation attempt
  await page.waitForTimeout(2000);
  await browser.close();
}

run().catch((error) => {
  console.error('[playwright] Failed:', error);
  process.exitCode = 1;
});
