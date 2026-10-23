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
    expect(await page.evaluate(() => typeof globalThis.NuiGraphReview?.analyzeDiagram)).toBe("function");
    const browserReview = await page.evaluate(() => globalThis.NuiGraphReview.analyzeDiagram({
      nodes: [{ key: "lonely", title: "" }],
      edges: []
    }));
    expect(browserReview.findings.map((finding) => finding.code)).toEqual([
      "missing_node_title",
      "orphan_node"
    ]);
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
    expect(cachedPaths).toContain("/diagram-review.js");

    const title = page.locator("#diagram-title");
    await title.focus();
    await page.keyboard.press("Control+k");
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
    await expect(page.getByRole("combobox", { name: "Search commands" })).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(title).toBeFocused();

    await page.keyboard.press("Control+k");
    await page.getByRole("combobox", { name: "Search commands" }).fill("definitely no such command");
    await expect(page.getByText("No matching commands", { exact: true })).toBeVisible();
    await page.keyboard.press("Enter");
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
    await page.keyboard.press("Escape");

    await page.keyboard.press("Control+k");
    await page.getByRole("combobox", { name: "Search commands" }).fill("keyboard shortcuts");
    await expect(page.getByRole("option", { name: /Show keyboard shortcuts/ })).toBeVisible();
    await page.keyboard.press("Enter");
    await expect(page.getByRole("dialog", { name: "Keyboard shortcuts" })).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.getByRole("dialog", { name: "Keyboard shortcuts" })).not.toBeVisible();

    let sharingTestId = null;
    try {
      await page.locator("#new-diagram").click();
      await expect.poll(async () => page.evaluate(async () => {
        const response = await fetch("/api/diagrams", { credentials: "same-origin" });
        const data = await response.json();
        return data.diagrams?.[0]?.id || null;
      })).not.toBeNull();
      sharingTestId = await page.evaluate(async () => {
        const response = await fetch("/api/diagrams", { credentials: "same-origin" });
        return (await response.json()).diagrams[0].id;
      });

      await expect(page.locator("#share-diagram")).toBeEnabled();
      await expect(page.locator("#share-diagram")).toHaveAttribute("title", "Enable sharing");

      const unsafeNodeTitle = 'Service <img src=x onerror="globalThis.__nodeNavigatorXss=1">';
      await page.evaluate(async ({ id, title }) => {
        const session = await (await fetch("/api/session", { credentials: "same-origin" })).json();
        const diagram = await (await fetch(`/api/diagrams/${id}`, { credentials: "same-origin" })).json();
        diagram.nodes.find((node) => node.key === "node_service").title = title;
        const response = await fetch(`/api/diagrams/${id}`, {
          method: "PUT",
          credentials: "same-origin",
          headers: {
            "Content-Type": "application/json",
            "X-CSRF-Token": session.csrf_token,
            "If-Match": `"diagram-${id}-r${diagram.revision}"`
          },
          body: JSON.stringify(diagram)
        });
        if (!response.ok) throw new Error(`Could not prepare navigator fixture: ${response.status}`);
      }, { id: sharingTestId, title: unsafeNodeTitle });
      await page.reload({ waitUntil: "networkidle" });
      await expect(page.locator("#share-diagram")).toBeEnabled();

      await page.locator("#share-diagram").focus();
      await page.keyboard.press("/");
      const nodeNavigator = page.getByRole("dialog", { name: "Node navigator" });
      await expect(nodeNavigator).toBeVisible();
      const nodeSearch = nodeNavigator.getByRole("combobox", { name: "Search nodes" });
      await expect(nodeSearch).toBeFocused();
      await nodeSearch.fill("node_service");
      const serviceOption = nodeNavigator.getByRole("option", { name: /Service.*node_service/ });
      await expect(serviceOption).toBeVisible();
      await expect(serviceOption).toContainText(unsafeNodeTitle);
      await expect(serviceOption.locator("img")).toHaveCount(0);
      expect(await page.evaluate(() => globalThis.__nodeNavigatorXss)).toBeUndefined();
      await page.keyboard.press("Enter");
      await expect(nodeNavigator).not.toBeVisible();
      await expect(page.locator('#nodes > g[data-key="node_service"]')).toHaveClass(/selected/);
      await expect(page.locator("#share-diagram")).toBeFocused();

      await page.keyboard.press("/");
      await nodeSearch.fill("definitely no such node");
      await expect(nodeNavigator.getByText("No matching nodes", { exact: true })).toBeVisible();
      await page.keyboard.press("Enter");
      await expect(nodeNavigator).toBeVisible();
      await page.keyboard.press("Escape");
      await expect(nodeNavigator).not.toBeVisible();
      await expect(page.locator("#share-diagram")).toBeFocused();

      await page.locator("#share-diagram").click();
      const sharingDialog = page.getByRole("dialog", { name: "Share diagram" });
      await expect(sharingDialog).toBeVisible();
      await expect(sharingDialog.getByText("Private", { exact: true })).toBeVisible();
      await sharingDialog.getByRole("button", { name: "Enable share link" }).click();
      const shareInput = sharingDialog.getByRole("textbox", { name: "Active share link" });
      await expect(shareInput).toBeVisible();
      const firstLink = await shareInput.inputValue();
      expect(firstLink).toMatch(/\/d\/[a-z0-9-]+-[0-9a-f]{32}$/);

      page.once("dialog", (dialog) => dialog.accept());
      await sharingDialog.getByRole("button", { name: "Rotate link" }).click();
      await expect.poll(() => shareInput.inputValue()).not.toBe(firstLink);
      const rotatedLink = await shareInput.inputValue();

      page.once("dialog", (dialog) => dialog.accept());
      await sharingDialog.getByRole("button", { name: "Disable sharing" }).click();
      await expect(sharingDialog.getByText("Private", { exact: true })).toBeVisible();
      const revokedStatuses = await page.evaluate(async (links) => Promise.all(links.map(async (link) => {
        const path = new URL(link).pathname.replace(/^\/d\//, "/api/diagrams/slug/");
        return (await fetch(path, { credentials: "same-origin" })).status;
      })), [firstLink, rotatedLink]);
      expect(revokedStatuses).toEqual([404, 404]);
      await sharingDialog.getByRole("button", { name: "Close" }).click();
    } finally {
      if (sharingTestId) {
        await page.evaluate(async (id) => {
          const session = await (await fetch("/api/session", { credentials: "same-origin" })).json();
          const response = await fetch(`/api/diagrams/${id}`, { credentials: "same-origin" });
          if (!response.ok) return;
          const diagram = await response.json();
          await fetch(`/api/diagrams/${id}`, {
            method: "DELETE",
            credentials: "same-origin",
            headers: {
              "X-CSRF-Token": session.csrf_token,
              "If-Match": `"diagram-${id}-r${diagram.revision}"`
            }
          });
        }, sharingTestId);
      }
    }

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

  test("keeps the canvas usable with a customisable touch action bar", async ({ page }) => {
    await installPolicyViolationRecorder(page);
    await page.goto("/", { waitUntil: "networkidle" });

    await expect(page.locator("#canvas")).toBeVisible();
    await expect(page.locator(".mobile-action-bar")).toBeVisible();
    await expect(page.locator(".top-actions")).not.toBeVisible();
    await expect(page.locator(".toolbox")).not.toBeVisible();
    await expect(page.getByRole("button", { name: "Diagrams", exact: true })).toBeVisible();
    await expect(page.getByRole("button", { name: "Commands", exact: true })).toBeVisible();
    await expect(page.getByRole("button", { name: "Properties", exact: true })).toBeVisible();

    const geometry = await page.evaluate(() => ({
      viewportWidth: window.innerWidth,
      documentWidth: document.documentElement.scrollWidth,
      canvas: document.querySelector("#canvas").getBoundingClientRect().toJSON(),
      targets: [...document.querySelectorAll(".mobile-action-bar button")].map((button) =>
        button.getBoundingClientRect().height)
    }));
    expect(geometry.documentWidth).toBeLessThanOrEqual(geometry.viewportWidth);
    expect(geometry.canvas.width).toBeGreaterThan(300);
    expect(geometry.canvas.height).toBeGreaterThan(400);
    expect(geometry.targets.every((height) => height >= 44)).toBe(true);

    await page.getByRole("button", { name: "Diagrams", exact: true }).click();
    await expect(page.locator(".project-panel")).toBeVisible();
    await expect(page.locator(".sheet-scrim")).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(page.locator(".project-panel")).not.toBeVisible();

    await page.getByRole("button", { name: "Commands", exact: true }).click();
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
    await page.getByRole("combobox", { name: "Search commands" }).fill("customize mobile");
    await page.keyboard.press("Enter");
    const customizer = page.getByRole("dialog", { name: "Customize mobile action bar" });
    await expect(customizer).toBeVisible();
    await expect(customizer.getByLabel("Find and focus node", { exact: true })).toBeVisible();
    await customizer.getByLabel("Add node", { exact: true }).uncheck();
    await customizer.getByLabel("Zoom to fit", { exact: true }).check();
    await customizer.getByRole("button", { name: "Apply", exact: true }).click();
    await expect(page.getByRole("button", { name: "Zoom to fit", exact: true })).toBeVisible();

    await page.reload({ waitUntil: "networkidle" });
    await expect(page.getByRole("button", { name: "Zoom to fit", exact: true })).toBeVisible();
    await expect(page.locator(".mobile-action-bar button")).toHaveCount(5);

    expect(await page.evaluate(() => globalThis.__nuigraphPolicyViolations)).toEqual([]);
  });
});
