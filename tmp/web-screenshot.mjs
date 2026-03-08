import playwrightModule from '../source/tools/uapmd-app/web/node_modules/playwright/index.mjs';

const { firefox } = playwrightModule;

async function main() {
  const browser = await firefox.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  page.on('console', (msg) => console.log('[browser]', msg.type(), msg.text()));
  await page.goto('http://localhost:8080', { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => typeof Module !== 'undefined' && typeof Module.__uapmdUnlockAudio === 'function', null, { timeout: 60000 });
  await page.evaluate(() => Module.__uapmdUnlockAudio());
  await page.waitForTimeout(5000);
  await page.screenshot({ path: 'tmp/uapmd-ui.png', fullPage: true });
  await browser.close();
  console.log('Screenshot saved to tmp/uapmd-ui.png');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
