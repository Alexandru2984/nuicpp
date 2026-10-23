const { defineConfig } = require("@playwright/test");

const baseURL = process.env.NUIGRAPH_BROWSER_BASE_URL;
if (!baseURL) {
  throw new Error("NUIGRAPH_BROWSER_BASE_URL is required");
}

module.exports = defineConfig({
  testDir: "./tests",
  testMatch: "browser.spec.cjs",
  fullyParallel: false,
  workers: 1,
  retries: 0,
  timeout: 30_000,
  expect: { timeout: 8_000 },
  reporter: "line",
  outputDir: process.env.NUIGRAPH_BROWSER_OUTPUT_DIR || "test-results",
  use: {
    baseURL,
    serviceWorkers: "allow",
    trace: "retain-on-failure"
  }
});
