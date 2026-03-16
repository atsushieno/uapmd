const { test, expect } = require('@playwright/test');

test('audio engine starts after first user gesture', async ({ page }) => {
  const consoleMessages = [];
  page.on('console', (msg) => {
    const entry = `${msg.type()}: ${msg.text()}`;
    consoleMessages.push(entry);
    console.log('[console]', entry);
  });

  await page.goto('/');

  await page.waitForFunction(
    () => typeof window !== 'undefined' && window.Module && window.Module.calledRun === true,
    null,
    { timeout: 60_000 },
  );

  const isolation = await page.evaluate(() => ({
    crossOriginIsolated:
      typeof crossOriginIsolated === 'undefined' ? 'undefined' : String(crossOriginIsolated),
  }));
  console.log('[test] isolation', isolation);

  const workletStarted = page.waitForEvent('console', {
    predicate: (msg) =>
      msg.type() === 'log' && msg.text().includes('[WebAudioWorklet] AudioWorklet started'),
    timeout: 20_000,
  });

  await page.mouse.click(32, 32);
  await page.waitForTimeout(1000);
  const errors = await page.evaluate(() => window._uapmdAudioWorkletErrors || []);
  console.log('[test] worklet errors', errors);

  await workletStarted;

  // Give the audio thread a little time to run to catch latent crashes that
  // happen shortly after boot (e.g. first process() call).
  await page.waitForTimeout(2000);
  const abortMessage = consoleMessages.find((line) =>
    line.includes('Aborted(native code called abort())'),
  );
  expect(abortMessage).toBeFalsy();

  const missingExport = consoleMessages.find(
    (line) =>
      line.includes('uapmd_start_audio') &&
      (line.includes('missing') || line.toLowerCase().includes('bind')),
  );
  expect(missingExport).toBeFalsy();
});
