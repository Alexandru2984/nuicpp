const { test, expect, devices } = require("@playwright/test");
const { defaultBrowserType: _defaultBrowserType, ...pixel5 } = devices["Pixel 5"];

async function installPolicyViolationRecorder(page) {
  await page.addInitScript(() => {
    globalThis.__nuigraphPolicyViolations = [];
    document.addEventListener("securitypolicyviolation", (event) => {
      globalThis.__nuigraphPolicyViolations.push({
        directive: event.effectiveDirective,
        blockedURI: event.blockedURI
      });
    });
  });
}

async function waitForControlledWorker(page) {
  await page.evaluate(async () => {
    await navigator.serviceWorker.ready;
    if (navigator.serviceWorker.controller) return;
    await new Promise((resolve) => {
      navigator.serviceWorker.addEventListener("controllerchange", resolve, { once: true });
    });
  });
}

test.describe("desktop security and offline lifecycle", () => {
  test.use({ viewport: { width: 1440, height: 900 } });

  test("enforces CSP and replaces stale shell caches without caching APIs", async ({ page, context, baseURL }) => {
    await installPolicyViolationRecorder(page);

    // Establish the origin on an endpoint that does not load app.js, then
    // plant an obsolete release cache before the first worker can register.
    // Its activation must remove that cache.
    await page.goto("/health", { waitUntil: "domcontentloaded" });
    await page.evaluate(async () => {
      const stale = await caches.open("ngs-obsolete-browser-test");
      await stale.put("/obsolete-browser-test", new Response("obsolete"));
    });

    const response = await page.goto("/", { waitUntil: "networkidle" });
    expect(response).not.toBeNull();

    const csp = response.headers()["content-security-policy"] || "";
    expect(csp).toContain("default-src 'self'");
    expect(csp).not.toContain("'unsafe-inline'");
    expect(csp).not.toContain("'unsafe-eval'");
    expect(csp).not.toContain("'wasm-unsafe-eval'");

    await expect(page.locator("#canvas")).toBeVisible();
    await expect(page.locator("#diagram-title")).toHaveValue("Untitled diagram");
    await waitForControlledWorker(page);

    await expect.poll(async () => page.evaluate(() => caches.keys())).toEqual([
      expect.stringMatching(/^ngs-[0-9a-f]{16}$/)
    ]);

    await page.evaluate(() => fetch("/api/session", { credentials: "same-origin" }));
    const cachedPaths = await page.evaluate(async () => {
      const paths = [];
      for (const cacheName of await caches.keys()) {
        const cache = await caches.open(cacheName);
        for (const request of await cache.keys()) {
          paths.push(new URL(request.url).pathname);
        }
      }
      return paths;
    });
    expect(cachedPaths.some((path) => path.startsWith("/api/"))).toBe(false);
    expect(cachedPaths).toContain("/offline.html");

    expect(await page.evaluate(() => globalThis.__nuigraphPolicyViolations)).toEqual([]);

    await context.setOffline(true);
    try {
      await page.goto(`${baseURL}/browser-test-offline`, { waitUntil: "domcontentloaded" });
      await expect(page).toHaveTitle("NuiGraph Studio is offline");
      await expect(page.getByRole("heading", { name: "Offline" })).toBeVisible();
      await expect(page.getByRole("button", { name: "Try again" })).toBeVisible();
    } finally {
      await context.setOffline(false);
    }
  });
});

test.describe("phone layout", () => {
  test.use(pixel5);

  test("keeps the canvas usable and exposes touch-sized bottom sheets", async ({ page }) => {
    await installPolicyViolationRecorder(page);
    await page.goto("/", { waitUntil: "networkidle" });

    await expect(page.locator("#canvas")).toBeVisible();
    await expect(page.locator(".sheet-toggles")).toBeVisible();
    await expect(page.getByRole("button", { name: "Diagrams", exact: true })).toBeVisible();
    await expect(page.getByRole("button", { name: "Properties", exact: true })).toBeVisible();

    const geometry = await page.evaluate(() => ({
      viewportWidth: window.innerWidth,
      documentWidth: document.documentElement.scrollWidth,
      canvas: document.querySelector("#canvas").getBoundingClientRect().toJSON(),
      targets: [...document.querySelectorAll(".sheet-toggles button")].map((button) =>
        button.getBoundingClientRect().height)
    }));
    expect(geometry.documentWidth).toBeLessThanOrEqual(geometry.viewportWidth);
    expect(geometry.canvas.width).toBeGreaterThan(300);
    expect(geometry.canvas.height).toBeGreaterThan(250);
    expect(geometry.targets.every((height) => height >= 44)).toBe(true);

    await page.getByRole("button", { name: "Diagrams", exact: true }).click();
    await expect(page.locator(".project-panel")).toBeVisible();
    await expect(page.locator(".sheet-scrim")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator(".project-panel")).not.toBeVisible();

    expect(await page.evaluate(() => globalThis.__nuigraphPolicyViolations)).toEqual([]);
  });
});
