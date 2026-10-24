const { test, expect, devices } = require("@playwright/test");
const AxeBuilder = require("@axe-core/playwright").default;
const { defaultBrowserType: _defaultBrowserType, ...pixel5 } = devices["Pixel 5"];

async function expectNoHighImpactAccessibilityViolations(page, testInfo, label) {
  const results = await new AxeBuilder({ page }).analyze();
  const violations = results.violations
    .filter((violation) => violation.impact === "serious" || violation.impact === "critical")
    .map((violation) => ({
      id: violation.id,
      impact: violation.impact,
      help: violation.help,
      helpUrl: violation.helpUrl,
      nodes: violation.nodes.map((node) => ({
        target: node.target,
        html: node.html,
        failureSummary: node.failureSummary
      }))
    }));
  if (violations.length) {
    await testInfo.attach(`axe-${label}.json`, {
      body: Buffer.from(JSON.stringify(violations, null, 2)),
      contentType: "application/json"
    });
  }
  expect(violations, `${label} has serious or critical axe violations`).toEqual([]);
}

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

  test("enforces CSP and replaces stale shell caches without caching APIs", async ({ page, context, baseURL }, testInfo) => {
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
    const editorLiveStatus = page.locator("#editor-live-status");
    await expect(editorLiveStatus).toHaveAttribute("role", "status");
    await expect(editorLiveStatus).toHaveAttribute("aria-live", "polite");
    await expect(page.locator("#canvas")).toHaveAttribute("aria-describedby", "editor-live-status");
    await expect(page.locator("#draft-status")).toHaveAttribute("aria-live", "polite");
    await expectNoHighImpactAccessibilityViolations(page, testInfo, "desktop-editor");
    await page.emulateMedia({ forcedColors: "active", reducedMotion: "reduce" });
    await page.locator("#canvas").focus();
    const accessibilityMedia = await page.evaluate(() => ({
      forcedColors: matchMedia("(forced-colors: active)").matches,
      reducedMotion: matchMedia("(prefers-reduced-motion: reduce)").matches,
      canvasOutline: getComputedStyle(document.querySelector("#canvas")).outlineStyle,
      activeOutline: getComputedStyle(document.querySelector("#snap-toggle")).outlineStyle
    }));
    expect(accessibilityMedia).toEqual({
      forcedColors: true,
      reducedMotion: true,
      canvasOutline: "solid",
      activeOutline: "solid"
    });
    await page.emulateMedia({ forcedColors: "none", reducedMotion: "no-preference" });
    await page.locator("#grid-size").selectOption("64");
    await expect(page.locator("#grid")).toHaveAttribute("width", "64");
    await expect(page.locator("#grid")).toHaveAttribute("height", "64");
    await expect(page.locator("#grid path")).toHaveAttribute("d", "M 64 0 L 0 0 0 64");
    expect(await page.evaluate(() => localStorage.getItem("nuigraph:grid-size:v1"))).toBe("64");
    expect(await page.evaluate(() => typeof globalThis.NuiGraphReview?.analyzeDiagram)).toBe("function");
    expect(await page.evaluate(() => typeof globalThis.NuiGraphArrange?.arrangeNodes)).toBe("function");
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
    expect(cachedPaths).toContain("/diagram-arrange.js");
    expect(cachedPaths).toContain("/diagram-selection.js");

    const title = page.locator("#diagram-title");
    await title.focus();
    await page.keyboard.press("Control+k");
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
    await expect(page.getByRole("combobox", { name: "Search commands" })).toBeFocused();
    await expectNoHighImpactAccessibilityViolations(page, testInfo, "command-palette");
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
    const shortcutsDialog = page.getByRole("dialog", { name: "Keyboard shortcuts" });
    const shortcutsClose = shortcutsDialog.getByRole("button", { name: "Close" });
    await expect(shortcutsDialog).toBeVisible();
    await expect(shortcutsClose).toBeFocused();
    await page.keyboard.press("Tab");
    await expect(shortcutsClose).toBeFocused();
    await page.keyboard.press("Shift+Tab");
    await expect(shortcutsClose).toBeFocused();
    await page.keyboard.press("Escape");
    await expect(shortcutsDialog).not.toBeVisible();
    await expect(title).toBeFocused();

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
      const unsafeDiagramTitle = 'Diagram <img src=x onerror="globalThis.__diagramListXss=1">';
      const maxLengthNodeKey = "n".repeat(64);
      const maxLengthEdgeKey = "e".repeat(64);
      await page.evaluate(async ({ id, title, diagramTitle, longNodeKey, longEdgeKey }) => {
        const session = await (await fetch("/api/session", { credentials: "same-origin" })).json();
        const diagram = await (await fetch(`/api/diagrams/${id}`, { credentials: "same-origin" })).json();
        diagram.title = diagramTitle;
        diagram.nodes.find((node) => node.key === "node_service").title = title;
        const decision = diagram.nodes.find((node) => node.key === "node_decision");
        diagram.nodes.push({
          key: "node_isolated",
          type: "note",
          title: "Disconnected note",
          x: 820,
          y: 320,
          width: 180,
          height: 72,
          color: "#fde047",
          metadata: {}
        });
        for (const edge of diagram.edges) {
          if (edge.source === decision.key) edge.source = longNodeKey;
          if (edge.target === decision.key) edge.target = longNodeKey;
        }
        decision.key = longNodeKey;
        diagram.edges[0].key = longEdgeKey;
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
      }, {
        id: sharingTestId,
        title: unsafeNodeTitle,
        diagramTitle: unsafeDiagramTitle,
        longNodeKey: maxLengthNodeKey,
        longEdgeKey: maxLengthEdgeKey
      });
      await page.reload({ waitUntil: "networkidle" });
      await expect(page.locator("#share-diagram")).toBeEnabled();
      await expect(page.locator("#grid-size")).toHaveValue("64");
      await expect(page.locator("#grid")).toHaveAttribute("width", "64");
      const currentDiagramListItem = page.locator("#diagram-list .diagram-item.active");
      await expect(currentDiagramListItem.locator(".diagram-name")).toHaveText(unsafeDiagramTitle);
      await expect(currentDiagramListItem.locator("img")).toHaveCount(0);
      expect(await page.evaluate(() => globalThis.__diagramListXss)).toBeUndefined();

      await page.locator("#share-diagram").focus();
      const diagramNodeCount = await page.locator("#nodes > g").count();
      expect(diagramNodeCount).toBeGreaterThan(1);
      await page.keyboard.press("Control+a");
      await expect(page.locator("#nodes > g.selected")).toHaveCount(diagramNodeCount);
      await expect(editorLiveStatus).toHaveText(`${diagramNodeCount} nodes selected.`);
      await expect(page.locator("#canvas")).toBeFocused();
      const diagramEdgeCount = await page.locator("#edges > g").count();
      await page.keyboard.press("Control+d");
      await expect(page.locator("#nodes > g")).toHaveCount(diagramNodeCount * 2);
      await expect(page.locator("#edges > g")).toHaveCount(diagramEdgeCount * 2);
      await expect(page.locator("#nodes > g.selected")).toHaveCount(diagramNodeCount);
      const duplicateIntegrity = await page.evaluate(({ longNodeKey, longEdgeKey }) => {
        const nodeKeys = [...document.querySelectorAll("#nodes > g")].map((node) => node.dataset.key);
        const edgeKeys = [...document.querySelectorAll("#edges > g")].map((edge) => edge.dataset.key);
        return {
          nodeKeys,
          edgeKeys,
          longNodeFamily: nodeKeys.filter((key) => key.startsWith(longNodeKey.slice(0, 24))).length,
          longEdgeFamily: edgeKeys.filter((key) => key.startsWith(longEdgeKey.slice(0, 24))).length,
          decisionCopies: [...document.querySelectorAll("#nodes .node-title")]
            .filter((title) => title.textContent === "Decision Copy").length
        };
      }, { longNodeKey: maxLengthNodeKey, longEdgeKey: maxLengthEdgeKey });
      expect(new Set(duplicateIntegrity.nodeKeys).size).toBe(duplicateIntegrity.nodeKeys.length);
      expect(new Set(duplicateIntegrity.edgeKeys).size).toBe(duplicateIntegrity.edgeKeys.length);
      expect(duplicateIntegrity.nodeKeys.every((key) => /^[A-Za-z0-9_-]{1,64}$/.test(key))).toBe(true);
      expect(duplicateIntegrity.edgeKeys.every((key) => /^[A-Za-z0-9_-]{1,64}$/.test(key))).toBe(true);
      expect(duplicateIntegrity.longNodeFamily).toBe(2);
      expect(duplicateIntegrity.longEdgeFamily).toBe(2);
      expect(duplicateIntegrity.decisionCopies).toBe(1);
      await page.keyboard.press("Control+z");
      await expect(page.locator("#nodes > g")).toHaveCount(diagramNodeCount);
      await expect(page.locator("#edges > g")).toHaveCount(diagramEdgeCount);

      const firstEdgeKey = await page.locator("#edges > g").first().getAttribute("data-key");
      expect(firstEdgeKey).toBeTruthy();
      await page.keyboard.press("Alt+o");
      const diagramOutline = page.getByRole("dialog", { name: "Diagram outline" });
      await expect(diagramOutline).toBeVisible();
      const outlineFilter = diagramOutline.getByRole("searchbox", { name: "Filter diagram objects" });
      await expect(outlineFilter).toBeFocused();
      await expect(diagramOutline.getByRole("tree", { name: "Diagram objects" })).toBeVisible();
      await outlineFilter.fill(firstEdgeKey);
      const edgeTreeItem = diagramOutline.getByRole("treeitem", { name: new RegExp(firstEdgeKey) });
      await expect(edgeTreeItem).toHaveAttribute("aria-level", "1");
      await page.keyboard.press("ArrowDown");
      await expect(edgeTreeItem).toBeFocused();
      await page.keyboard.press("Enter");
      await expect(diagramOutline).not.toBeVisible();
      await expect(page.locator(`#edges > g[data-key="${firstEdgeKey}"]`)).toHaveClass(/edge-selected/);
      await expect(editorLiveStatus).toContainText("Edge selected:");
      await expect(page.locator("#canvas")).toBeFocused();

      await page.keyboard.press("Alt+o");
      await outlineFilter.fill("node_service");
      const outlineService = diagramOutline.locator('.diagram-outline-item[data-object-type="node"]');
      await expect(outlineService).toContainText(unsafeNodeTitle);
      await expect(outlineService.locator("img")).toHaveCount(0);
      expect(await page.evaluate(() => globalThis.__nodeNavigatorXss)).toBeUndefined();
      await outlineService.focus();
      await page.keyboard.press("Enter");
      await expect(diagramOutline).not.toBeVisible();
      await expect(page.locator('#nodes > g[data-key="node_service"]')).toHaveClass(/selected/);
      await expect(editorLiveStatus).toContainText(`Node selected: ${unsafeNodeTitle}`);
      await expect(editorLiveStatus.locator("img")).toHaveCount(0);
      await expect(page.locator("#canvas")).toBeFocused();

      await page.keyboard.press("Control+k");
      const connectedPalette = page.getByRole("dialog", { name: "Command palette" });
      await connectedPalette.getByRole("combobox", { name: "Search commands" }).fill("select connected component");
      await expect(connectedPalette.getByRole("option", { name: /Select connected component/ })).toBeEnabled();
      await page.keyboard.press("Enter");
      await expect(connectedPalette).not.toBeVisible();
      await expect(page.locator("#nodes > g.selected")).toHaveCount(diagramNodeCount - 1);
      await expect(page.locator('#nodes > g[data-key="node_isolated"]')).not.toHaveClass(/selected/);
      await expect(editorLiveStatus).toHaveText(`${diagramNodeCount - 1} nodes selected.`);
      await expect(page.locator("#canvas")).toBeFocused();

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
      await expect(page.locator("#canvas")).toBeFocused();

      await page.keyboard.press("/");
      await nodeSearch.fill("definitely no such node");
      await expect(nodeNavigator.getByText("No matching nodes", { exact: true })).toBeVisible();
      await page.keyboard.press("Enter");
      await expect(nodeNavigator).toBeVisible();
      await page.keyboard.press("Escape");
      await expect(nodeNavigator).not.toBeVisible();
      await expect(page.locator("#canvas")).toBeFocused();

      const serviceGroup = page.locator('#nodes > g[data-key="node_service"]');
      const originalRenderedTitle = await serviceGroup.locator(".node-title").textContent();
      await page.keyboard.press("F2");
      const labelEditor = page.getByRole("dialog", { name: "Edit node label" });
      const labelInput = labelEditor.getByRole("textbox", { name: "Node label" });
      await expect(labelEditor).toBeVisible();
      await expect(labelInput).toBeFocused();
      await expect(labelInput).toHaveValue(unsafeNodeTitle);

      await labelInput.fill("   ");
      await labelEditor.getByRole("button", { name: "Save" }).click();
      await expect(labelEditor).toBeVisible();
      expect(await labelInput.evaluate((input) => input.validationMessage)).not.toBe("");

      const editedNodeTitle = '<img src=x onerror="globalThis.__inlineLabelXss=1"> Renamed';
      await labelInput.fill(editedNodeTitle);
      await labelEditor.getByRole("button", { name: "Save" }).click();
      await expect(labelEditor).not.toBeVisible();
      await expect(serviceGroup.locator(".node-title")).not.toHaveText(originalRenderedTitle);
      expect(await page.evaluate(() => globalThis.__inlineLabelXss)).toBeUndefined();
      expect(await serviceGroup.evaluate((group) => group.querySelectorAll("img").length)).toBe(0);

      await page.keyboard.press("Control+z");
      await expect(serviceGroup.locator(".node-title")).toHaveText(originalRenderedTitle);
      await serviceGroup.dispatchEvent("dblclick", { bubbles: true });
      await expect(labelEditor).toBeVisible();
      await expect(labelInput).toHaveValue(unsafeNodeTitle);
      await page.keyboard.press("Escape");
      await expect(labelEditor).not.toBeVisible();
      await page.locator("#canvas").focus();
      await page.keyboard.press("Enter");
      await expect(labelEditor).toBeVisible();
      await labelEditor.getByRole("button", { name: "Cancel" }).click();
      await expect(labelEditor).not.toBeVisible();

      const startGroup = page.locator('#nodes > g[data-key="node_start"]');
      const startShape = startGroup.locator(".node-shape");
      const serviceShape = serviceGroup.locator(".node-shape");
      const originalServiceX = await serviceShape.getAttribute("x");
      await expect(serviceGroup).toHaveClass(/selected/);
      await startShape.dispatchEvent("pointerdown", {
        bubbles: true,
        clientX: 100,
        clientY: 100,
        pointerId: 919,
        pointerType: "mouse",
        shiftKey: true
      });
      await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointerup", {
        bubbles: true,
        pointerId: 919,
        pointerType: "mouse"
      })));
      await expect(startGroup).toHaveClass(/selected/);
      await expect(serviceGroup).toHaveClass(/selected/);
      await expect(editorLiveStatus).toHaveText("2 nodes selected.");

      await page.keyboard.press("Control+k");
      const commandPalette = page.getByRole("dialog", { name: "Command palette" });
      await commandPalette.getByRole("combobox", { name: "Search commands" }).fill("align selected nodes left");
      await expect(commandPalette.getByRole("option", { name: /Align selected nodes left/ })).toBeEnabled();
      await page.keyboard.press("Enter");
      await expect(commandPalette).not.toBeVisible();
      await expect(serviceShape).toHaveAttribute("x", await startShape.getAttribute("x"));

      await page.keyboard.press("Control+z");
      await expect(serviceShape).toHaveAttribute("x", originalServiceX);

      const originalBatchProperties = {
        startType: await startGroup.locator(".node-subtitle").textContent(),
        serviceType: await serviceGroup.locator(".node-subtitle").textContent(),
        startFill: await startShape.getAttribute("fill"),
        serviceFill: await serviceShape.getAttribute("fill"),
        startWidth: await startShape.getAttribute("width"),
        serviceWidth: await serviceShape.getAttribute("width")
      };
      await serviceShape.dispatchEvent("pointerdown", {
        bubbles: true,
        clientX: 300,
        clientY: 100,
        detail: 1,
        pointerId: 921,
        pointerType: "mouse"
      });
      await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointerup", {
        bubbles: true,
        pointerId: 921,
        pointerType: "mouse"
      })));
      await startShape.dispatchEvent("pointerdown", {
        bubbles: true,
        clientX: 100,
        clientY: 100,
        detail: 1,
        pointerId: 922,
        pointerType: "mouse",
        shiftKey: true
      });
      await page.evaluate(() => window.dispatchEvent(new PointerEvent("pointerup", {
        bubbles: true,
        pointerId: 922,
        pointerType: "mouse"
      })));

      const batchInspector = page.locator("#multi-node-props");
      await expect(batchInspector).toBeVisible();
      await expect(page.locator("#multi-node-count")).toHaveText("2 nodes selected");
      await expect(page.locator("#multi-node-summary")).toContainText("bounds");
      await page.locator("#zoom-out").click();
      await page.locator("#zoom-out").click();
      const zoomBeforeFrame = Number.parseInt(await page.locator("#zoom-label").textContent(), 10);
      await page.locator("#canvas").focus();
      await page.keyboard.press("Shift+2");
      const zoomAfterFrame = Number.parseInt(await page.locator("#zoom-label").textContent(), 10);
      expect(zoomAfterFrame).toBeGreaterThan(zoomBeforeFrame);
      await page.locator("#batch-type").selectOption("api");
      await page.locator("#batch-color-enabled").check();
      await page.locator("#batch-color").fill("#123456");
      await page.locator("#batch-width-enabled").check();
      await page.locator("#batch-width").fill("222");
      await page.locator("#apply-batch-properties").click();
      await expect(startGroup.locator(".node-subtitle")).toHaveText("api");
      await expect(serviceGroup.locator(".node-subtitle")).toHaveText("api");
      await expect(startShape).toHaveAttribute("fill", "#123456");
      await expect(serviceShape).toHaveAttribute("fill", "#123456");
      await expect(startShape).toHaveAttribute("width", "222");
      await expect(serviceShape).toHaveAttribute("width", "222");

      const startXBeforeNudge = Number(await startShape.getAttribute("x"));
      const serviceXBeforeNudge = Number(await serviceShape.getAttribute("x"));
      await page.locator("#canvas").focus();
      await page.keyboard.press("ArrowRight");
      await page.keyboard.press("ArrowRight");
      await expect(startShape).toHaveAttribute("x", String(startXBeforeNudge + 128));
      await expect(serviceShape).toHaveAttribute("x", String(serviceXBeforeNudge + 128));
      await page.keyboard.press("Control+z");
      await expect(startShape).toHaveAttribute("x", String(startXBeforeNudge));
      await expect(serviceShape).toHaveAttribute("x", String(serviceXBeforeNudge));
      await expect(startShape).toHaveAttribute("fill", "#123456");

      await page.keyboard.press("Control+z");
      await expect(startGroup.locator(".node-subtitle")).toHaveText(originalBatchProperties.startType);
      await expect(serviceGroup.locator(".node-subtitle")).toHaveText(originalBatchProperties.serviceType);
      await expect(startShape).toHaveAttribute("fill", originalBatchProperties.startFill);
      await expect(serviceShape).toHaveAttribute("fill", originalBatchProperties.serviceFill);
      await expect(startShape).toHaveAttribute("width", originalBatchProperties.startWidth);
      await expect(serviceShape).toHaveAttribute("width", originalBatchProperties.serviceWidth);

      const edgeGroup = page.locator("#edges > g").first();
      const originalEdgeLabel = await edgeGroup.locator(".edge-label").textContent();
      await edgeGroup.locator(".edge-hit").dispatchEvent("pointerdown", {
        bubbles: true,
        detail: 1,
        pointerId: 920,
        pointerType: "mouse"
      });
      await expect(edgeGroup).toHaveClass(/edge-selected/);
      await page.keyboard.press("F2");
      const edgeLabelEditor = page.getByRole("dialog", { name: "Edit edge label" });
      const edgeLabelInput = edgeLabelEditor.getByRole("textbox", { name: "Edge label" });
      await expect(edgeLabelEditor).toBeVisible();
      await expect(edgeLabelInput).toHaveValue(originalEdgeLabel || "");

      const editedEdgeLabel = '<img src=x onerror="globalThis.__inlineEdgeLabelXss=1"> Flow';
      await edgeLabelInput.fill(editedEdgeLabel);
      await edgeLabelEditor.getByRole("button", { name: "Save" }).click();
      await expect(edgeLabelEditor).not.toBeVisible();
      await expect(edgeGroup.locator(".edge-label")).toBeVisible();
      const editedRenderedEdgeLabel = await edgeGroup.locator(".edge-label").textContent();
      expect(await page.evaluate(() => globalThis.__inlineEdgeLabelXss)).toBeUndefined();
      expect(await edgeGroup.evaluate((group) => group.querySelectorAll("img").length)).toBe(0);

      await edgeGroup.dispatchEvent("dblclick", { bubbles: true });
      await edgeLabelInput.fill("   ");
      await edgeLabelEditor.getByRole("button", { name: "Save" }).click();
      await expect(edgeGroup.locator(".edge-label")).toHaveCount(0);

      await page.keyboard.press("Control+z");
      await expect(edgeGroup.locator(".edge-label")).toHaveText(editedRenderedEdgeLabel);
      await page.keyboard.press("Control+z");
      if (originalEdgeLabel === null) {
        await expect(edgeGroup.locator(".edge-label")).toHaveCount(0);
      } else {
        await expect(edgeGroup.locator(".edge-label")).toHaveText(originalEdgeLabel);
      }
      await edgeGroup.dispatchEvent("dblclick", { bubbles: true });
      await expect(edgeLabelEditor).toBeVisible();
      await edgeLabelEditor.getByRole("button", { name: "Cancel" }).click();

      await page.locator("#share-diagram").click();
      const sharingDialog = page.getByRole("dialog", { name: "Share diagram" });
      await expect(sharingDialog).toBeVisible();
      await expect(sharingDialog.getByText("Private", { exact: true })).toBeVisible();
      const lifetime = sharingDialog.getByRole("combobox", { name: "Share link lifetime" });
      await expect(lifetime).toHaveValue("604800");
      await sharingDialog.getByRole("button", { name: "Enable share link" }).click();
      const shareInput = sharingDialog.getByRole("textbox", { name: "Active share link" });
      await expect(shareInput).toBeVisible();
      await expect(sharingDialog.getByText(/can view and fork the diagram until/)).toBeVisible();
      const firstLink = await shareInput.inputValue();
      expect(firstLink).toMatch(/\/d\/[a-z0-9-]+-[0-9a-f]{32}$/);

      await lifetime.selectOption("never");
      await sharingDialog.getByRole("button", { name: "Update expiry" }).click();
      await expect(sharingDialog.getByText(/no automatic expiry/i)).toBeVisible();

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

  test("keeps the canvas usable with a customisable touch action bar", async ({ page }, testInfo) => {
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
    await expectNoHighImpactAccessibilityViolations(page, testInfo, "mobile-editor");

    const diagramsButton = page.getByRole("button", { name: "Diagrams", exact: true });
    await diagramsButton.click();
    await expect(page.locator(".project-panel")).toBeVisible();
    await expect(page.locator(".sheet-scrim")).toBeVisible();
    await expect(page.locator(".project-panel .sheet-close")).toBeFocused();
    await expectNoHighImpactAccessibilityViolations(page, testInfo, "mobile-diagrams-sheet");
    await page.keyboard.press("Shift+Tab");
    expect(await page.evaluate(() => document.querySelector(".project-panel").contains(document.activeElement))).toBe(true);
    await page.keyboard.press("Escape");
    await expect(page.locator(".project-panel")).not.toBeVisible();
    await expect(diagramsButton).toBeFocused();

    const propertiesButton = page.getByRole("button", { name: "Properties", exact: true });
    await propertiesButton.click();
    await page.evaluate(() => {
      document.querySelector("#selection-empty").classList.add("hidden");
      document.querySelector("#multi-node-props").classList.remove("hidden");
      document.querySelector("#multi-node-count").textContent = "2 nodes selected";
    });
    await expect(page.locator("#multi-node-props")).toBeVisible();
    await expect(page.locator("#multi-node-count")).toHaveText("2 nodes selected");
    expect(await page.locator("#apply-batch-properties").evaluate((button) =>
      button.getBoundingClientRect().height)).toBeGreaterThanOrEqual(44);
    await page.keyboard.press("Escape");
    await expect(page.locator(".properties")).not.toBeVisible();
    await expect(propertiesButton).toBeFocused();

    await page.getByRole("button", { name: "Commands", exact: true }).click();
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
    await page.getByRole("combobox", { name: "Search commands" }).fill("customize mobile");
    await page.keyboard.press("Enter");
    const customizer = page.getByRole("dialog", { name: "Customize mobile action bar" });
    await expect(customizer).toBeVisible();
    await expect(customizer.getByLabel("Find and focus node", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Edit selected label", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Frame selection", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Cycle grid size", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Align selected nodes left", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Select all nodes", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Select connected component", { exact: true })).toBeVisible();
    await expect(customizer.getByLabel("Duplicate selected nodes", { exact: true })).toBeVisible();
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
