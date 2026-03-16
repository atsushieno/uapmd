import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  timeout: 120 * 1000,
  expect: {
    timeout: 15 * 1000,
  },
  retries: process.env.CI ? 1 : 0,
  webServer: {
    command: 'npm run serve',
    url: 'http://localhost:8080',
    reuseExistingServer: false,
    timeout: 120 * 1000,
    stdout: 'pipe',
    stderr: 'pipe',
  },
  use: {
    baseURL: 'http://localhost:8080',
    browserName: 'chromium',
    headless: true,
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
    launchOptions: {
      args: [
        '--use-gl=swiftshader',
        '--use-angle=swiftshader',
        '--ignore-gpu-blocklist',
        '--enable-unsafe-swiftshader',
      ],
    },
  },
});
