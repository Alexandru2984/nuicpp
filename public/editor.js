(() => {
  const assetQuery = document.currentScript ? new URL(document.currentScript.src).search : "";
  if (!document.querySelector('link[href^="/styles.css"]')) {
    const css = document.createElement("link");
    css.rel = "stylesheet";
    css.href = `/styles.css${assetQuery}`;
    document.head.appendChild(css);
  }

  const svgNS = "http://www.w3.org/2000/svg";
  const nodeTypes = ["process", "decision", "database", "service", "api", "note", "external"];
  const GRID_SIZES = [8, 16, 32, 64];
  const GRID_SIZE_STORAGE = "nuigraph:grid-size:v1";
  const colors = {
    process: "#38bdf8",
    decision: "#f59e0b",
    database: "#a78bfa",
    service: "#22c55e",
    api: "#60a5fa",
    note: "#fde047",
    external: "#fb7185"
  };

  const state = {
    diagrams: [],
    diagramsTotal: 0,
    diagramsHasMore: false,
    templates: [],
    current: null,
    selected: null,
    readOnly: false,
    tool: "select",
    snap: true,
    gridSize: 16,
    zoom: 1,
    pan: { x: 80, y: 70 },
    edgeSource: null,
    csrfToken: null,
    history: [],
    future: [],
    dragging: null,
    panning: null,
    clipboard: null,
    dirty: false,
    draftTimer: null,
    // Pointer Events unify mouse, touch and pen. Tracking every active pointer
    // by id is what makes two-finger pinch zoom possible; a single-pointer
    // gesture keeps behaving exactly like the old mouse handlers.
    pointers: new Map(),
    pinch: null,
    multiSelectArmed: false,
    longPressTimer: null,
    marquee: null,
    resizing: null,
    guides: [],
    filter: ""
  };

  const el = (id) => document.getElementById(id);
  const canvas = el("canvas");
  const viewport = el("viewport");
  const nodesLayer = el("nodes");
  const edgesLayer = el("edges");
  // A selected canvas object needs a real keyboard focus target before Enter
  // can be a safe, predictable editing shortcut.
  canvas.tabIndex = 0;
  const editorLiveStatus = document.createElement("div");
  editorLiveStatus.id = "editor-live-status";
  editorLiveStatus.className = "sr-only";
  editorLiveStatus.setAttribute("role", "status");
  editorLiveStatus.setAttribute("aria-live", "polite");
  editorLiveStatus.setAttribute("aria-atomic", "true");
  document.body.appendChild(editorLiveStatus);
  canvas.setAttribute("aria-describedby", editorLiveStatus.id);
  const draftLiveStatus = el("draft-status");
  if (draftLiveStatus) {
    draftLiveStatus.setAttribute("role", "status");
    draftLiveStatus.setAttribute("aria-live", "polite");
    draftLiveStatus.setAttribute("aria-atomic", "true");
  }
  // Both shells ship this layer, but a browser holding a cached copy of an
  // older Nui bundle would not have it, and a missing overlay must not take the
  // whole canvas down. svg() is a hoisted declaration, so this is safe here.
  const overlayLayer = el("overlay") || svg("g", { id: "overlay" }, viewport);

  async function ensureCsrfToken() {
    if (state.csrfToken) return state.csrfToken;
    const res = await fetch("/api/session", { credentials: "same-origin" });
    if (res.status === 401) {
      location.href = "/login";
      throw new Error("authentication required");
    }
    const data = await res.json();
    state.csrfToken = data.csrf_token;
    return state.csrfToken;
  }

  async function api(path, options = {}) {
    const method = (options.method || "GET").toUpperCase();
    const { headers: requestedHeaders = {}, ...fetchOptions } = options;
    const headers = { ...requestedHeaders };
    if (fetchOptions.body !== undefined && !Object.keys(headers).some(
      (name) => name.toLowerCase() === "content-type"
    )) {
      headers["Content-Type"] = "application/json";
    }
    if (!["GET", "HEAD", "OPTIONS"].includes(method)) {
      headers["X-CSRF-Token"] = await ensureCsrfToken();
    }
    return fetch(path, {
      ...fetchOptions,
      credentials: "same-origin",
      headers
    }).then(async (res) => {
      if (res.status === 401) {
        location.href = "/login";
        return Promise.reject(new Error("authentication required"));
      }
      const text = await res.text();
      const data = text ? JSON.parse(text) : {};
      if (!res.ok) {
        const error = new Error(data.error || res.statusText);
        error.status = res.status;
        error.data = data;
        throw error;
      }
      return data;
    });
  }

  function diagramEtag(diagram) {
    if (!Number.isSafeInteger(diagram?.id) || diagram.id <= 0 ||
        !Number.isSafeInteger(diagram?.revision) || diagram.revision <= 0) {
      throw new Error("Diagram revision is unavailable; reopen the diagram before changing it.");
    }
    return `"diagram-${diagram.id}-r${diagram.revision}"`;
  }

  function handleRevisionConflict(error, preserveCurrentDraft = true,
                                  cleanMessage = "A newer revision was saved elsewhere. Refresh the diagram list before deleting it.") {
    if (error?.status !== 412) return false;
    if (preserveCurrentDraft) {
      state.dirty = true;
      saveDraftNow();
      setDraftStatus("Conflict - reopen required", true);
    }
    const message = preserveCurrentDraft
      ? "A newer revision was saved elsewhere. Reopen the diagram before saving again; this browser draft was preserved."
      : cleanMessage;
    toast(message, "error");
    return true;
  }


  // ------------------------------------------------------------- toasts --
  // alert() blocks the page and looks like a browser error; these are
  // non-blocking and stack.
  function toastStack() {
    let stack = document.querySelector(".toast-stack");
    if (!stack) {
      stack = document.createElement("div");
      stack.className = "toast-stack";
      stack.setAttribute("role", "status");
      stack.setAttribute("aria-live", "polite");
      document.body.appendChild(stack);
    }
    return stack;
  }

  function toast(message, kind = "") {
    const node = document.createElement("div");
    node.className = `toast ${kind}`.trim();
    node.textContent = message;
    toastStack().appendChild(node);
    window.setTimeout(() => node.remove(), kind === "error" ? 6000 : 3200);
  }

  // ------------------------------------------------------- mobile sheets --
  // Below the phone breakpoint the diagram list and properties panel are
  // off-canvas sheets. The scrim and close controls are built here so the
  // server-rendered and Nui shells keep the same minimal markup.
  let sheetControls = {
    open: () => {},
    closeAll: () => {}
  };

  function initSheets() {
    const panels = {
      diagrams: document.querySelector(".project-panel"),
      properties: document.querySelector(".properties")
    };
    let lastTrigger = null;

    const scrim = document.createElement("button");
    scrim.className = "sheet-scrim hidden";
    scrim.setAttribute("aria-label", "Close panel");
    document.body.appendChild(scrim);

    const closeAll = (restoreFocus = true) => {
      Object.values(panels).forEach((panel) => {
        panel?.classList.remove("sheet-open");
        panel?.removeAttribute("aria-modal");
        panel?.removeAttribute("role");
        panel?.removeAttribute("aria-label");
      });
      document.querySelectorAll(".mobile-action-bar [aria-expanded]").forEach((button) => {
        button.setAttribute("aria-expanded", "false");
      });
      scrim.classList.add("hidden");
      if (restoreFocus && lastTrigger?.isConnected) lastTrigger.focus();
      lastTrigger = null;
    };

    const open = (name, trigger = null) => {
      const panel = panels[name];
      if (!panel) return;
      if (!window.matchMedia("(max-width: 760px)").matches) {
        const target = [...panel.querySelectorAll("input, button, select")]
          .find((node) => node.offsetParent !== null);
        (target || panel).focus();
        return;
      }
      const alreadyOpen = panel.classList.contains("sheet-open");
      closeAll(false);
      if (alreadyOpen) {
        trigger?.focus();
        return;
      }
      lastTrigger = trigger?.isConnected ? trigger : document.activeElement;
      panel.classList.add("sheet-open");
      panel.setAttribute("role", "dialog");
      panel.setAttribute("aria-modal", "true");
      panel.setAttribute("aria-label", name === "diagrams" ? "Diagrams panel" : "Properties panel");
      scrim.classList.remove("hidden");
      const toggle = document.querySelector(`.mobile-action-bar [data-panel="${name}"]`);
      toggle?.setAttribute("aria-expanded", "true");
      panel.querySelector(".sheet-close")?.focus();
    };

    for (const [name, panel] of Object.entries(panels)) {
      if (!panel) continue;
      panel.id ||= `mobile-sheet-${name}`;
      panel.tabIndex = -1;

      const close = document.createElement("button");
      close.type = "button";
      close.className = "sheet-close";
      close.textContent = "\u00d7";
      close.setAttribute("aria-label", "Close panel");
      close.title = "Close";
      close.onclick = closeAll;
      panel.prepend(close);
    }

    scrim.onclick = closeAll;
    window.addEventListener("keydown", (evt) => {
      const openPanel = Object.values(panels).find((panel) => panel?.classList.contains("sheet-open"));
      if (evt.key === "Escape" && openPanel) {
        evt.preventDefault();
        evt.stopImmediatePropagation();
        closeAll();
      } else if (openPanel) {
        trapDialogFocus(evt, openPanel);
      }
    });

    sheetControls = { open, closeAll };
  }

  // -------------------------------------------------------------- theme --
  function initTheme() {
    const stored = localStorage.getItem("nuigraph:theme");
    if (stored === "light" || stored === "dark") {
      document.documentElement.dataset.theme = stored;
    }

    const button = document.createElement("button");
    button.type = "button";
    button.id = "theme-toggle";
    button.title = "Toggle light and dark theme";
    const paint = () => {
      const dark = document.documentElement.dataset.theme
        ? document.documentElement.dataset.theme === "dark"
        : !window.matchMedia("(prefers-color-scheme: light)").matches;
      button.textContent = dark ? "Light" : "Dark";
      button.setAttribute("aria-label", dark ? "Switch to light theme" : "Switch to dark theme");
    };
    button.onclick = () => {
      const dark = document.documentElement.dataset.theme
        ? document.documentElement.dataset.theme === "dark"
        : !window.matchMedia("(prefers-color-scheme: light)").matches;
      const next = dark ? "light" : "dark";
      document.documentElement.dataset.theme = next;
      localStorage.setItem("nuigraph:theme", next);
      paint();
    };
    paint();
    document.querySelector(".top-actions")?.prepend(button);
  }


  // ------------------------------------------------------ search + help --
  // Injected rather than added to the server-rendered markup so the shell keeps
  // working unchanged if this script fails to load.
  function initSearch() {
    const panel = document.querySelector(".project-panel");
    const list = el("diagram-list");
    if (!panel || !list) return;

    const wrap = document.createElement("div");
    wrap.className = "search-wrap";
    const input = document.createElement("input");
    input.type = "search";
    input.id = "diagram-search";
    input.placeholder = "Search diagrams";
    input.setAttribute("aria-label", "Search diagrams");
    input.autocomplete = "off";
    input.oninput = () => {
      state.filter = input.value;
      renderDiagramList();
    };
    wrap.appendChild(input);
    list.parentNode.insertBefore(wrap, list);
  }

  // ------------------------------------------------------ command palette --
  // Commands are static definitions and all labels are assigned with
  // textContent. Search text never becomes markup or code.
  const MOBILE_ACTION_STORAGE = "nuigraph:mobile-actions:v1";
  const DEFAULT_MOBILE_ACTIONS = ["save", "tool-node"];
  let mobileActionBar = null;

  function triggerButton(id) {
    const button = el(id);
    if (!button || button.disabled) return;
    button.click();
  }

  function triggerTool(tool) {
    const button = document.querySelector(`.toolbox button[data-tool="${tool}"]`);
    if (!button || button.disabled) return;
    button.click();
  }

  function commandDefinitions() {
    return [
      { id: "new", label: "New diagram", shortLabel: "New", category: "Diagram", keywords: "create blank", mobile: true, run: newDiagram },
      { id: "sample", label: "Create from cloud template", shortLabel: "Sample", category: "Diagram", keywords: "sample architecture", run: loadSample },
      { id: "save", label: "Save diagram", shortLabel: "Save", category: "Diagram", keywords: "persist sync", shortcut: "Ctrl / Cmd + S", mobile: true, enabled: () => Boolean(state.current) && !state.readOnly, run: saveDiagram },
      { id: "share", label: "Manage or copy sharing", shortLabel: "Share", category: "Diagram", keywords: "private public url revoke rotate expire timed link", mobile: true, enabled: () => Boolean(state.current), run: shareDiagram },
      { id: "fork", label: "Fork shared diagram", shortLabel: "Fork", category: "Diagram", keywords: "copy public link own", mobile: true, enabled: () => Boolean(state.current) && state.readOnly && Boolean(sharedSlugFromPath()), run: forkSharedDiagram },
      { id: "undo", label: "Undo", shortLabel: "Undo", category: "Edit", shortcut: "Ctrl / Cmd + Z", mobile: true, enabled: () => state.history.length > 0 && !state.readOnly, run: undo },
      { id: "redo", label: "Redo", shortLabel: "Redo", category: "Edit", shortcut: "Ctrl / Cmd + Y", mobile: true, enabled: () => state.future.length > 0 && !state.readOnly, run: redo },
      { id: "copy", label: "Copy selection", shortLabel: "Copy", category: "Edit", shortcut: "Ctrl / Cmd + C", enabled: () => selectedNodeKeys().length > 0, run: copySelected },
      { id: "cut", label: "Cut selected nodes", shortLabel: "Cut", category: "Edit", keywords: "move clipboard remove nodes edges", shortcut: "Ctrl / Cmd + X", mobile: true, enabled: () => selectedNodeKeys().length > 0 && !state.readOnly, run: cutSelected },
      { id: "paste", label: "Paste selection", shortLabel: "Paste", category: "Edit", shortcut: "Ctrl / Cmd + V", enabled: () => Boolean(state.clipboard?.nodes?.length) && !state.readOnly, run: pasteSelected },
      { id: "duplicate-selection", label: "Duplicate selected nodes", shortLabel: "Duplicate", category: "Edit", keywords: "clone copy nodes edges", shortcut: "Ctrl / Cmd + D", mobile: true, enabled: () => selectedNodeKeys().length > 0 && !state.readOnly, run: duplicateSelected },
      { id: "select-all", label: "Select all nodes", shortLabel: "All", category: "Edit", keywords: "multi selection everything batch", shortcut: "Ctrl / Cmd + A", mobile: true, enabled: () => Boolean(state.current?.nodes?.length), run: selectAllNodes },
      // The persisted mobile-action id stays stable as edge support expands the command.
      { id: "edit-node-label", label: "Edit selected label", shortLabel: "Rename", category: "Edit", keywords: "inline rename node edge title", shortcut: "F2 / Enter", mobile: true, enabled: () => Boolean(editableSelectedLabelTarget()) && !state.readOnly, run: openInlineLabelEditor },
      { id: "delete", label: "Delete selection", shortLabel: "Delete", category: "Edit", mobile: true, enabled: () => Boolean(state.selected) && !state.readOnly, run: deleteSelected },
      { id: "layout", label: "Auto layout", shortLabel: "Layout", category: "Arrange", keywords: "organize graph", mobile: true, enabled: () => Boolean(state.current?.nodes?.length) && !state.readOnly, run: autoLayout },
      { id: "align-left", label: "Align selected nodes left", shortLabel: "Left", category: "Arrange", keywords: "multi selection position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-left") },
      { id: "align-center", label: "Align selected nodes horizontally", shortLabel: "H center", category: "Arrange", keywords: "multi selection center position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-center") },
      { id: "align-right", label: "Align selected nodes right", shortLabel: "Right", category: "Arrange", keywords: "multi selection position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-right") },
      { id: "align-top", label: "Align selected nodes top", shortLabel: "Top", category: "Arrange", keywords: "multi selection position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-top") },
      { id: "align-middle", label: "Align selected nodes vertically", shortLabel: "V middle", category: "Arrange", keywords: "multi selection center position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-middle") },
      { id: "align-bottom", label: "Align selected nodes bottom", shortLabel: "Bottom", category: "Arrange", keywords: "multi selection position", mobile: true, enabled: () => canArrangeSelection(2), run: () => arrangeSelection("align-bottom") },
      { id: "distribute-horizontal", label: "Distribute selected nodes horizontally", shortLabel: "Space H", category: "Arrange", keywords: "multi selection equal spacing", mobile: true, enabled: () => canArrangeSelection(3), run: () => arrangeSelection("distribute-horizontal") },
      { id: "distribute-vertical", label: "Distribute selected nodes vertically", shortLabel: "Space V", category: "Arrange", keywords: "multi selection equal spacing", mobile: true, enabled: () => canArrangeSelection(3), run: () => arrangeSelection("distribute-vertical") },
      { id: "toggle-geometry-lock", label: "Toggle node geometry lock", shortLabel: "Lock", category: "Arrange", keywords: "position size freeze unlock protect layout", mobile: true, enabled: () => selectedNodeKeys().length > 0 && !state.readOnly, run: toggleSelectedGeometryLock },
      { id: "review", label: "Review diagram quality", shortLabel: "Review", category: "Analyze", keywords: "lint cycle orphan duplicate missing label", mobile: true, enabled: () => Boolean(state.current), run: showDiagramReview },
      { id: "select-sources", label: "Select source nodes", shortLabel: "Sources", category: "Analyze", keywords: "entry roots indegree incoming architecture", mobile: true, enabled: () => Boolean(state.current?.nodes?.length), run: () => selectTerminalNodes("source") },
      { id: "select-sinks", label: "Select sink nodes", shortLabel: "Sinks", category: "Analyze", keywords: "output leaves outdegree outgoing architecture", mobile: true, enabled: () => Boolean(state.current?.nodes?.length), run: () => selectTerminalNodes("sink") },
      { id: "select-neighbours", label: "Select direct neighbors", shortLabel: "Neighbors", category: "Analyze", keywords: "adjacent one hop graph incoming outgoing neighbours expand selection", mobile: true, enabled: () => selectedNodeKeys().length > 0, run: selectNeighbourNodes },
      { id: "select-connected", label: "Select connected component", shortLabel: "Connected", category: "Analyze", keywords: "graph traversal reachable weak component expand selection neighbours", mobile: true, enabled: () => selectedNodeKeys().length > 0, run: selectConnectedNodes },
      { id: "find-node", label: "Find and focus node", shortLabel: "Find", category: "Analyze", keywords: "navigate search locate jump", shortcut: "/", mobile: true, enabled: () => Boolean(state.current?.nodes?.length), run: openNodeNavigator },
      { id: "diagram-outline", label: "Open diagram outline", shortLabel: "Outline", category: "Analyze", keywords: "accessibility screen reader tree nodes edges navigate", shortcut: "Alt + O", mobile: true, enabled: () => Boolean(state.current?.nodes?.length || state.current?.edges?.length), run: openDiagramOutline },
      { id: "frame-selection", label: "Frame selection", shortLabel: "Frame", category: "View", keywords: "zoom center focus selected nodes edge", shortcut: "Shift + 2", mobile: true, enabled: () => selectedFrameNodes().length > 0, run: frameSelection },
      { id: "fit", label: "Zoom to fit", shortLabel: "Fit", category: "View", keywords: "frame center canvas", mobile: true, enabled: () => Boolean(state.current?.nodes?.length), run: zoomToFit },
      { id: "tool-select", label: "Use select tool", shortLabel: "Select", category: "Tools", keywords: "pointer", mobile: true, run: () => triggerTool("select") },
      { id: "tool-pan", label: "Use pan tool", shortLabel: "Pan", category: "Tools", keywords: "move canvas hand", mobile: true, run: () => triggerTool("pan") },
      { id: "tool-node", label: "Add node", shortLabel: "Node", category: "Tools", keywords: "create shape", mobile: true, enabled: () => Boolean(state.current) && !state.readOnly, run: () => triggerTool("node") },
      { id: "tool-edge", label: "Connect nodes with an edge", shortLabel: "Edge", category: "Tools", keywords: "line arrow", mobile: true, enabled: () => Boolean(state.current) && !state.readOnly, run: () => triggerTool("edge") },
      { id: "snap", label: "Toggle grid snapping", shortLabel: "Snap", category: "Arrange", keywords: "align grid", run: () => triggerButton("snap-toggle") },
      { id: "grid-size", label: "Cycle grid size", shortLabel: "Grid", category: "Arrange", keywords: "snap spacing 8 16 32 64", mobile: true, run: cycleGridSize },
      { id: "zoom-in", label: "Zoom in", shortLabel: "Zoom +", category: "View", enabled: () => state.zoom < MAX_ZOOM, run: () => triggerButton("zoom-in") },
      { id: "zoom-out", label: "Zoom out", shortLabel: "Zoom -", category: "View", enabled: () => state.zoom > MIN_ZOOM, run: () => triggerButton("zoom-out") },
      { id: "theme", label: "Toggle light or dark theme", shortLabel: "Theme", category: "View", keywords: "appearance", run: () => triggerButton("theme-toggle") },
      { id: "panel-diagrams", label: "Open diagrams panel", shortLabel: "Diagrams", category: "Panels", keywords: "projects templates", run: () => sheetControls.open("diagrams") },
      { id: "panel-properties", label: "Open properties panel", shortLabel: "Properties", category: "Panels", keywords: "inspect versions", run: () => sheetControls.open("properties") },
      { id: "import", label: "Import JSON", shortLabel: "Import", category: "Import and export", enabled: () => !state.readOnly, run: () => triggerButton("import-json") },
      { id: "export-json", label: "Export JSON", shortLabel: "JSON", category: "Import and export", enabled: () => Boolean(state.current) && !el("export-json")?.disabled, run: () => triggerButton("export-json") },
      { id: "export-svg", label: "Export SVG", shortLabel: "SVG", category: "Import and export", enabled: () => Boolean(state.current), run: exportSvg },
      { id: "export-png", label: "Export PNG", shortLabel: "PNG", category: "Import and export", enabled: () => Boolean(state.current), run: exportPng },
      { id: "export-mermaid", label: "Export Mermaid", shortLabel: "Mermaid", category: "Import and export", enabled: () => Boolean(state.current), run: exportMermaid },
      { id: "shortcuts", label: "Show keyboard shortcuts", shortLabel: "Shortcuts", category: "Help", keywords: "help keys", shortcut: "?", run: toggleShortcuts },
      { id: "customize-mobile", label: "Customize mobile action bar", shortLabel: "Customize", category: "View", keywords: "pin bottom buttons", run: openMobileCustomizer }
    ];
  }

  function commandEnabled(command) {
    if (!command) return false;
    return command.enabled ? Boolean(command.enabled()) : true;
  }

  function executeCommand(command, onComplete = null) {
    if (!command || !commandEnabled(command)) return;
    try {
      const result = command.run();
      if (result && typeof result.then === "function") {
        result.catch((error) => toast(error.message || "Command failed.", "error"))
          .finally(() => {
            refreshMobileActionBar();
            onComplete?.();
          });
      } else {
        refreshMobileActionBar();
        onComplete?.();
      }
    } catch (error) {
      toast(error.message || "Command failed.", "error");
    }
  }

  function trapDialogFocus(evt, dialog) {
    if (evt.key !== "Tab") return;
    const selector = [
      'a[href]:not([tabindex="-1"])',
      'area[href]:not([tabindex="-1"])',
      'button:not(:disabled):not([tabindex="-1"])',
      'input:not(:disabled):not([type="hidden"]):not([tabindex="-1"])',
      'select:not(:disabled):not([tabindex="-1"])',
      'textarea:not(:disabled):not([tabindex="-1"])',
      'details > summary:first-of-type',
      '[tabindex]:not([tabindex="-1"])'
    ].join(", ");
    const focusable = [...dialog.querySelectorAll(selector)]
      .filter((node) => node.tabIndex >= 0 && node.offsetParent !== null && !node.closest("[inert]"));
    if (!focusable.length) {
      evt.preventDefault();
      dialog.focus?.();
      return;
    }
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (!dialog.contains(document.activeElement)) {
      evt.preventDefault();
      (evt.shiftKey ? last : first).focus();
    } else if (evt.shiftKey && document.activeElement === first) {
      evt.preventDefault();
      last.focus();
    } else if (!evt.shiftKey && document.activeElement === last) {
      evt.preventDefault();
      first.focus();
    }
  }

  function closeDialog(selector) {
    const overlay = document.querySelector(selector);
    if (typeof overlay?.closeDialog === "function") overlay.closeDialog();
    else overlay?.remove();
  }

  function openCommandPalette(initialQuery = "") {
    const existing = document.querySelector(".command-palette-overlay");
    if (existing) {
      existing.querySelector("input")?.focus();
      return;
    }
    closeDialog(".shortcuts-overlay");
    sheetControls.closeAll(false);

    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay";
    const dialog = document.createElement("section");
    dialog.className = "command-palette";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "command-palette-title");

    const head = document.createElement("div");
    head.className = "command-palette-head";
    const title = document.createElement("h2");
    title.id = "command-palette-title";
    title.textContent = "Command palette";
    const close = document.createElement("button");
    close.type = "button";
    close.className = "command-close";
    close.textContent = "Close";
    head.append(title, close);

    const input = document.createElement("input");
    input.type = "search";
    input.className = "command-search";
    input.placeholder = "Type a command...";
    input.autocomplete = "off";
    input.setAttribute("aria-label", "Search commands");
    input.setAttribute("role", "combobox");
    input.setAttribute("aria-autocomplete", "list");
    input.setAttribute("aria-controls", "command-results");
    input.setAttribute("aria-expanded", "true");
    input.value = initialQuery;

    const status = document.createElement("div");
    status.className = "command-status";
    status.setAttribute("aria-live", "polite");
    const list = document.createElement("div");
    list.id = "command-results";
    list.className = "command-results";
    list.setAttribute("role", "listbox");
    dialog.append(head, input, status, list);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    let visible = [];
    let activeIndex = 0;
    const closePalette = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = closePalette;
    close.onclick = closePalette;
    overlay.onclick = (evt) => { if (evt.target === overlay) closePalette(); };

    const updateActive = () => {
      [...list.querySelectorAll(".command-item")].forEach((button, index) => {
        const active = index === activeIndex;
        button.classList.toggle("active", active);
        button.setAttribute("aria-selected", String(active));
      });
      const active = list.querySelectorAll(".command-item")[activeIndex];
      if (active) {
        input.setAttribute("aria-activedescendant", active.id);
        active.scrollIntoView({ block: "nearest" });
      } else {
        input.removeAttribute("aria-activedescendant");
      }
    };

    const runVisible = (index) => {
      const command = visible[index];
      if (!commandEnabled(command)) return;
      closePalette();
      executeCommand(command);
    };

    const renderCommands = () => {
      const words = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
      visible = commandDefinitions().filter((command) => {
        const haystack = `${command.label} ${command.category} ${command.keywords || ""}`.toLowerCase();
        return words.every((word) => haystack.includes(word));
      });
      activeIndex = Math.min(activeIndex, Math.max(0, visible.length - 1));
      list.replaceChildren();
      for (const [index, command] of visible.entries()) {
        const enabled = commandEnabled(command);
        const button = document.createElement("button");
        button.type = "button";
        button.id = `command-option-${index}`;
        button.className = "command-item";
        button.setAttribute("role", "option");
        button.setAttribute("aria-disabled", String(!enabled));
        button.disabled = !enabled;

        const copy = document.createElement("span");
        copy.className = "command-copy";
        const label = document.createElement("strong");
        label.textContent = command.label;
        const category = document.createElement("small");
        category.textContent = enabled ? command.category : `${command.category} - unavailable`;
        copy.append(label, category);
        button.appendChild(copy);
        if (command.shortcut) {
          const shortcut = document.createElement("kbd");
          shortcut.textContent = command.shortcut;
          button.appendChild(shortcut);
        }
        button.onclick = () => runVisible(index);
        button.onpointerenter = () => {
          activeIndex = index;
          updateActive();
        };
        list.appendChild(button);
      }
      if (!visible.length) {
        const empty = document.createElement("div");
        empty.className = "command-empty";
        empty.textContent = "No matching commands";
        list.appendChild(empty);
      }
      status.textContent = `${visible.length} command${visible.length === 1 ? "" : "s"}`;
      updateActive();
    };

    input.oninput = () => {
      activeIndex = 0;
      renderCommands();
    };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closePalette();
        return;
      }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "k") {
        evt.preventDefault();
        evt.stopPropagation();
        closePalette();
        return;
      }
      if (evt.key === "ArrowDown" || evt.key === "ArrowUp") {
        evt.preventDefault();
        if (visible.length) {
          activeIndex = (activeIndex + (evt.key === "ArrowDown" ? 1 : -1) + visible.length) % visible.length;
          updateActive();
        }
        return;
      }
      if (evt.key === "Enter" && document.activeElement === input) {
        evt.preventDefault();
        runVisible(activeIndex);
        return;
      }
      trapDialogFocus(evt, dialog);
    });

    renderCommands();
    input.focus();
  }

  function mobileCandidateCommands() {
    return commandDefinitions().filter((command) => command.mobile);
  }

  function mobileActionIds() {
    try {
      const raw = localStorage.getItem(MOBILE_ACTION_STORAGE);
      if (raw === null) return [...DEFAULT_MOBILE_ACTIONS];
      const saved = JSON.parse(raw);
      if (!Array.isArray(saved)) return [...DEFAULT_MOBILE_ACTIONS];
      const allowed = new Set(mobileCandidateCommands().map((command) => command.id));
      return [...new Set(saved)].filter((id) => allowed.has(id)).slice(0, 2);
    } catch {
      return [...DEFAULT_MOBILE_ACTIONS];
    }
  }

  function mobileBarButton(label, className = "") {
    const button = document.createElement("button");
    button.type = "button";
    button.className = className;
    button.textContent = label;
    return button;
  }

  function refreshMobileActionBar() {
    if (!mobileActionBar) return;
    const commands = new Map(commandDefinitions().map((command) => [command.id, command]));
    mobileActionBar.querySelectorAll("[data-command-id]").forEach((button) => {
      const command = commands.get(button.dataset.commandId);
      if (!command) return;
      button.disabled = !commandEnabled(command);
      button.classList.toggle("active", command.id === `tool-${state.tool}`);
      button.classList.toggle("has-changes", command.id === "save" && state.dirty);
      let accessibleLabel = command.id === "save" && state.dirty
        ? "Save diagram - unsaved changes"
        : command.label;
      const visibleLabel = button.textContent.trim();
      if (visibleLabel && !accessibleLabel.toLowerCase().includes(visibleLabel.toLowerCase())) {
        accessibleLabel = `${visibleLabel}: ${accessibleLabel}`;
      }
      button.setAttribute("aria-label", accessibleLabel);
    });
  }

  function renderMobileActionBar() {
    if (!mobileActionBar) return;
    const commands = new Map(commandDefinitions().map((command) => [command.id, command]));
    mobileActionBar.replaceChildren();

    const diagrams = mobileBarButton("Diagrams");
    diagrams.dataset.panel = "diagrams";
    diagrams.setAttribute("aria-controls", "mobile-sheet-diagrams");
    diagrams.setAttribute("aria-expanded", "false");
    diagrams.onclick = () => sheetControls.open("diagrams", diagrams);
    mobileActionBar.appendChild(diagrams);

    const pinned = mobileActionIds().map((id) => commands.get(id)).filter(Boolean);
    const appendCommand = (command) => {
      const button = mobileBarButton(command.shortLabel);
      button.dataset.commandId = command.id;
      button.title = command.label;
      button.onclick = () => executeCommand(command);
      mobileActionBar.appendChild(button);
    };
    if (pinned[0]) appendCommand(pinned[0]);

    const palette = mobileBarButton("Commands", "mobile-command-trigger");
    palette.setAttribute("aria-label", "Commands");
    palette.title = "Open command palette";
    palette.onclick = () => openCommandPalette();
    mobileActionBar.appendChild(palette);

    if (pinned[1]) appendCommand(pinned[1]);

    const properties = mobileBarButton("Properties");
    properties.setAttribute("aria-label", "Properties");
    properties.dataset.panel = "properties";
    properties.setAttribute("aria-controls", "mobile-sheet-properties");
    properties.setAttribute("aria-expanded", "false");
    properties.onclick = () => sheetControls.open("properties", properties);
    mobileActionBar.appendChild(properties);
    refreshMobileActionBar();
  }

  function initMobileActionBar() {
    mobileActionBar = document.createElement("nav");
    mobileActionBar.className = "mobile-action-bar";
    mobileActionBar.setAttribute("aria-label", "Mobile editor actions");
    document.body.appendChild(mobileActionBar);
    renderMobileActionBar();
  }

  function openMobileCustomizer() {
    closeDialog(".command-palette-overlay");
    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay mobile-customizer-overlay";
    const dialog = document.createElement("section");
    dialog.className = "mobile-customizer";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "mobile-customizer-title");

    const title = document.createElement("h2");
    title.id = "mobile-customizer-title";
    title.textContent = "Customize mobile action bar";
    const intro = document.createElement("p");
    intro.className = "muted";
    intro.textContent = "Pin up to two actions between Diagrams, Commands and Properties.";
    const choices = document.createElement("div");
    choices.className = "mobile-action-choices";
    const selected = new Set(mobileActionIds());
    const status = document.createElement("div");
    status.className = "command-status";
    status.setAttribute("aria-live", "polite");

    const update = () => {
      status.textContent = `${selected.size} of 2 actions selected`;
    };
    for (const command of mobileCandidateCommands()) {
      const label = document.createElement("label");
      label.className = "mobile-action-choice";
      const input = document.createElement("input");
      input.type = "checkbox";
      input.checked = selected.has(command.id);
      input.onchange = () => {
        if (input.checked && selected.size >= 2) {
          input.checked = false;
          status.textContent = "Choose no more than two actions.";
          return;
        }
        if (input.checked) selected.add(command.id);
        else selected.delete(command.id);
        update();
      };
      const text = document.createElement("span");
      text.textContent = command.label;
      label.append(input, text);
      choices.appendChild(label);
    }

    const actions = document.createElement("div");
    actions.className = "mobile-customizer-actions";
    const reset = mobileBarButton("Reset defaults");
    const cancel = mobileBarButton("Cancel");
    const apply = mobileBarButton("Apply");
    apply.classList.add("primary");
    actions.append(reset, cancel, apply);
    dialog.append(title, intro, choices, status, actions);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    const close = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
      else mobileActionBar?.querySelector(".mobile-command-trigger")?.focus();
    };
    overlay.closeDialog = close;
    overlay.onclick = (evt) => { if (evt.target === overlay) close(); };
    cancel.onclick = close;
    reset.onclick = () => {
      try {
        localStorage.removeItem(MOBILE_ACTION_STORAGE);
        renderMobileActionBar();
        close();
      } catch {
        status.textContent = "Browser storage is unavailable; the action bar was not changed.";
      }
    };
    apply.onclick = () => {
      try {
        localStorage.setItem(MOBILE_ACTION_STORAGE, JSON.stringify([...selected]));
        renderMobileActionBar();
        close();
      } catch {
        status.textContent = "Browser storage is unavailable; the action bar was not changed.";
      }
    };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        close();
        return;
      }
      trapDialogFocus(evt, dialog);
    });
    update();
    choices.querySelector("input")?.focus();
  }

  function focusReviewTarget(target) {
    if (!state.current || !target) return false;
    let center = null;
    if (target.type === "node") {
      const node = state.current.nodes.find((candidate) => candidate.key === target.key);
      if (!node) return false;
      state.selected = { type: "node", key: node.key };
      center = nodeCenter(node);
    } else if (target.type === "edge") {
      const edge = state.current.edges.find((candidate) => candidate.key === target.key);
      if (!edge) return false;
      const source = state.current.nodes.find((node) => node.key === edge.source);
      const destination = state.current.nodes.find((node) => node.key === edge.target);
      if (!source || !destination) return false;
      const a = nodeCenter(source);
      const b = nodeCenter(destination);
      center = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
      state.selected = { type: "edge", key: edge.key };
    }
    if (!center) return false;
    state.edgeSource = null;
    state.zoom = Math.max(1, state.zoom);
    const rect = canvas.getBoundingClientRect();
    state.pan.x = rect.width / 2 - center.x * state.zoom;
    state.pan.y = rect.height / 2 - center.y * state.zoom;
    setTool("select");
    render();
    return true;
  }

  // Keep an empty search useful without attempting to mount an unbounded
  // number of controls. Filtering still considers every node and the live
  // status reports the full match count.
  const NODE_NAVIGATOR_RESULT_LIMIT = 100;

  function openNodeNavigator() {
    const nodes = Array.isArray(state.current?.nodes) ? state.current.nodes : [];
    if (!nodes.length) return;
    const existing = document.querySelector(".node-navigator-overlay");
    if (existing) {
      existing.querySelector("input")?.focus();
      return;
    }
    closeDialog(".command-palette-overlay");
    closeDialog(".shortcuts-overlay");
    sheetControls.closeAll(false);

    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay node-navigator-overlay";
    const dialog = document.createElement("section");
    dialog.className = "node-navigator";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "node-navigator-title");

    const head = document.createElement("div");
    head.className = "command-palette-head";
    const title = document.createElement("h2");
    title.id = "node-navigator-title";
    title.textContent = "Node navigator";
    const closeButton = document.createElement("button");
    closeButton.type = "button";
    closeButton.className = "command-close";
    closeButton.textContent = "Close";
    head.append(title, closeButton);

    const input = document.createElement("input");
    input.type = "search";
    input.className = "command-search";
    input.placeholder = "Search by title, type, or key...";
    input.autocomplete = "off";
    input.setAttribute("aria-label", "Search nodes");
    input.setAttribute("role", "combobox");
    input.setAttribute("aria-autocomplete", "list");
    input.setAttribute("aria-controls", "node-navigator-results");
    input.setAttribute("aria-expanded", "true");

    const status = document.createElement("div");
    status.className = "command-status";
    status.setAttribute("aria-live", "polite");
    const list = document.createElement("div");
    list.id = "node-navigator-results";
    list.className = "command-results";
    list.setAttribute("role", "listbox");
    dialog.append(head, input, status, list);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    let visible = [];
    let activeIndex = 0;
    const closeNavigator = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = closeNavigator;
    closeButton.onclick = closeNavigator;
    overlay.onclick = (evt) => { if (evt.target === overlay) closeNavigator(); };

    const updateActive = () => {
      const buttons = [...list.querySelectorAll(".node-navigator-item")];
      buttons.forEach((button, index) => {
        const active = index === activeIndex;
        button.classList.toggle("active", active);
        button.setAttribute("aria-selected", String(active));
      });
      const active = buttons[activeIndex];
      if (active) {
        input.setAttribute("aria-activedescendant", active.id);
        active.scrollIntoView({ block: "nearest" });
      } else {
        input.removeAttribute("aria-activedescendant");
      }
    };

    const chooseVisible = (index) => {
      const node = visible[index];
      if (node && focusReviewTarget({ type: "node", key: node.key })) closeNavigator();
    };

    const renderNodes = () => {
      const words = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
      const matching = nodes.filter((node) => {
        const haystack = `${node.title || ""} ${node.type || ""} ${node.key || ""}`.toLowerCase();
        return words.every((word) => haystack.includes(word));
      });
      visible = matching.slice(0, NODE_NAVIGATOR_RESULT_LIMIT);
      activeIndex = Math.min(activeIndex, Math.max(0, visible.length - 1));
      list.replaceChildren();
      for (const [index, node] of visible.entries()) {
        const button = document.createElement("button");
        button.type = "button";
        button.id = `node-navigator-option-${index}`;
        button.className = "command-item node-navigator-item";
        button.setAttribute("role", "option");

        const copy = document.createElement("span");
        copy.className = "command-copy";
        const label = document.createElement("strong");
        label.textContent = node.title || "Untitled node";
        const metadata = document.createElement("small");
        metadata.textContent = `${node.type || "node"} - ${node.key}`;
        copy.append(label, metadata);
        button.appendChild(copy);
        button.onclick = () => chooseVisible(index);
        button.onpointerenter = () => {
          activeIndex = index;
          updateActive();
        };
        list.appendChild(button);
      }
      if (!visible.length) {
        const empty = document.createElement("div");
        empty.className = "command-empty";
        empty.textContent = "No matching nodes";
        list.appendChild(empty);
      }
      status.textContent = matching.length > visible.length
        ? `Showing ${visible.length} of ${matching.length} nodes`
        : `${matching.length} node${matching.length === 1 ? "" : "s"}`;
      updateActive();
    };

    input.oninput = () => {
      activeIndex = 0;
      renderNodes();
    };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closeNavigator();
        return;
      }
      if (evt.key === "ArrowDown" || evt.key === "ArrowUp") {
        evt.preventDefault();
        if (visible.length) {
          activeIndex = (activeIndex + (evt.key === "ArrowDown" ? 1 : -1) + visible.length) % visible.length;
          updateActive();
        }
        return;
      }
      if (evt.key === "Enter" && document.activeElement === input) {
        evt.preventDefault();
        chooseVisible(activeIndex);
        return;
      }
      trapDialogFocus(evt, dialog);
    });

    renderNodes();
    input.focus();
  }

  // SVG conveys the visual graph, while this flat ARIA tree exposes the same
  // objects as ordinary focusable controls. It includes edges as well as nodes
  // and searches the complete graph, but caps each rendered result set so a
  // deliberately huge valid diagram cannot lock up assistive technology.
  const DIAGRAM_OUTLINE_RESULT_LIMIT = 250;

  function openDiagramOutline() {
    const nodes = Array.isArray(state.current?.nodes) ? state.current.nodes : [];
    const edges = Array.isArray(state.current?.edges) ? state.current.edges : [];
    if (!nodes.length && !edges.length) return;
    const existing = document.querySelector(".diagram-outline-overlay");
    if (existing) {
      existing.querySelector("input")?.focus();
      return;
    }
    closeDialog(".command-palette-overlay");
    closeDialog(".shortcuts-overlay");
    sheetControls.closeAll(false);

    const previousFocus = document.activeElement;
    const byKey = new Map(nodes.map((node) => [node.key, node]));
    const nodeName = (key) => byKey.get(key)?.title || key || "Unknown node";
    const objects = [
      ...nodes.map((node) => ({
        type: "node",
        key: node.key,
        label: node.title || "Untitled node",
        metadata: `${node.type || "node"} node - ${node.key}${nodeGeometryLocked(node) ? " - geometry locked" : ""}`,
        search: `${node.title || ""} ${node.type || ""} ${node.key || ""}${nodeGeometryLocked(node) ? " geometry locked" : ""}`
      })),
      ...edges.map((edge) => {
        const source = nodeName(edge.source);
        const target = nodeName(edge.target);
        const direction = edge.directed ? "Directed edge" : "Undirected edge";
        return {
          type: "edge",
          key: edge.key,
          label: edge.label || "Unlabelled edge",
          metadata: `${direction} - ${source} to ${target} - ${edge.key}`,
          search: `${edge.label || ""} ${edge.key || ""} ${edge.source || ""} ${edge.target || ""} ${source} ${target} ${direction}`
        };
      })
    ];

    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay diagram-outline-overlay";
    const dialog = document.createElement("section");
    dialog.className = "diagram-outline";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "diagram-outline-title");

    const head = document.createElement("div");
    head.className = "command-palette-head";
    const title = document.createElement("h2");
    title.id = "diagram-outline-title";
    title.textContent = "Diagram outline";
    const closeButton = document.createElement("button");
    closeButton.type = "button";
    closeButton.className = "command-close";
    closeButton.textContent = "Close";
    head.append(title, closeButton);

    const input = document.createElement("input");
    input.type = "search";
    input.className = "command-search";
    input.placeholder = "Filter nodes and edges...";
    input.autocomplete = "off";
    input.setAttribute("aria-label", "Filter diagram objects");
    input.setAttribute("aria-controls", "diagram-outline-tree");

    const status = document.createElement("div");
    status.className = "command-status";
    status.setAttribute("aria-live", "polite");
    const tree = document.createElement("div");
    tree.id = "diagram-outline-tree";
    tree.className = "command-results diagram-outline-tree";
    tree.setAttribute("role", "tree");
    tree.setAttribute("aria-label", "Diagram objects");
    dialog.append(head, input, status, tree);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    let visible = [];
    let activeIndex = 0;
    const closeOutline = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = closeOutline;
    closeButton.onclick = closeOutline;
    overlay.onclick = (evt) => { if (evt.target === overlay) closeOutline(); };

    const itemButtons = () => [...tree.querySelectorAll(".diagram-outline-item")];
    const updateActive = (focus = false) => {
      const buttons = itemButtons();
      activeIndex = Math.min(activeIndex, Math.max(0, buttons.length - 1));
      buttons.forEach((button, index) => {
        const active = index === activeIndex;
        button.classList.toggle("active", active);
        button.tabIndex = active ? 0 : -1;
      });
      const active = buttons[activeIndex];
      if (focus && active) {
        active.focus();
        active.scrollIntoView({ block: "nearest" });
      }
    };

    const chooseVisible = (index) => {
      const object = visible[index];
      if (object && focusReviewTarget({ type: object.type, key: object.key })) closeOutline();
    };

    const renderObjects = () => {
      const words = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
      const matching = objects.filter((object) => {
        const haystack = `${object.label} ${object.metadata} ${object.search}`.toLowerCase();
        return words.every((word) => haystack.includes(word));
      });
      visible = matching.slice(0, DIAGRAM_OUTLINE_RESULT_LIMIT);
      activeIndex = 0;
      tree.replaceChildren();
      for (const [index, object] of visible.entries()) {
        const button = document.createElement("button");
        button.type = "button";
        button.id = `diagram-outline-item-${index}`;
        button.className = "command-item diagram-outline-item";
        button.dataset.objectType = object.type;
        button.dataset.objectKey = object.key;
        button.setAttribute("role", "treeitem");
        button.setAttribute("aria-level", "1");
        const selected = state.selected?.type === object.type && state.selected?.key === object.key;
        button.setAttribute("aria-selected", String(selected));

        const copy = document.createElement("span");
        copy.className = "command-copy";
        const label = document.createElement("strong");
        label.textContent = object.label;
        const metadata = document.createElement("small");
        metadata.textContent = object.metadata;
        copy.append(label, metadata);
        button.appendChild(copy);
        button.onclick = () => chooseVisible(index);
        button.onfocus = () => {
          activeIndex = index;
          updateActive();
        };
        button.onpointerenter = () => {
          activeIndex = index;
          updateActive();
        };
        tree.appendChild(button);
      }
      if (!visible.length) {
        const empty = document.createElement("div");
        empty.className = "command-empty";
        empty.textContent = "No matching diagram objects";
        tree.appendChild(empty);
      }
      const nodeCount = matching.filter((object) => object.type === "node").length;
      const edgeCount = matching.length - nodeCount;
      const totals = `${nodeCount} node${nodeCount === 1 ? "" : "s"} and ${edgeCount} edge${edgeCount === 1 ? "" : "s"}`;
      status.textContent = matching.length > visible.length
        ? `Showing ${visible.length} of ${matching.length} objects (${totals}); refine the filter for more.`
        : totals;
      updateActive();
    };

    input.oninput = renderObjects;
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closeOutline();
        return;
      }
      const buttons = itemButtons();
      if ((evt.key === "ArrowDown" || evt.key === "ArrowUp") &&
          (document.activeElement === input || document.activeElement?.matches(".diagram-outline-item"))) {
        evt.preventDefault();
        if (buttons.length) {
          const delta = evt.key === "ArrowDown" ? 1 : -1;
          activeIndex = document.activeElement === input
            ? (delta > 0 ? 0 : buttons.length - 1)
            : (activeIndex + delta + buttons.length) % buttons.length;
          updateActive(true);
        }
        return;
      }
      if ((evt.key === "Home" || evt.key === "End") && document.activeElement?.matches(".diagram-outline-item")) {
        evt.preventDefault();
        activeIndex = evt.key === "Home" ? 0 : Math.max(0, buttons.length - 1);
        updateActive(true);
        return;
      }
      if ((evt.key === "Enter" || evt.key === " ") && document.activeElement?.matches(".diagram-outline-item")) {
        evt.preventDefault();
        chooseVisible(activeIndex);
        return;
      }
      trapDialogFocus(evt, dialog);
    });

    renderObjects();
    input.focus();
  }

  function showDiagramReview() {
    if (!state.current) return;
    const analyzer = globalThis.NuiGraphReview?.analyzeDiagram;
    if (typeof analyzer !== "function") {
      toast("Diagram review is unavailable; reload the editor assets.", "error");
      return;
    }
    const review = analyzer(state.current);
    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay diagram-review-overlay";
    const dialog = document.createElement("section");
    dialog.className = "diagram-review";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "diagram-review-title");

    const head = document.createElement("div");
    head.className = "command-palette-head";
    const heading = document.createElement("div");
    const title = document.createElement("h2");
    title.id = "diagram-review-title";
    title.textContent = "Diagram review";
    const subtitle = document.createElement("p");
    subtitle.className = "muted";
    subtitle.textContent = review.summary.total
      ? `${review.summary.total} actionable finding${review.summary.total === 1 ? "" : "s"}`
      : "No structural issues found";
    heading.append(title, subtitle);
    const closeButton = mobileBarButton("Close");
    head.append(heading, closeButton);

    const summary = document.createElement("div");
    summary.className = "review-summary";
    for (const severity of ["error", "warning", "info"]) {
      const badge = document.createElement("span");
      badge.className = `review-badge review-${severity}`;
      badge.textContent = `${review.summary[severity]} ${severity}${review.summary[severity] === 1 ? "" : "s"}`;
      summary.appendChild(badge);
    }

    const list = document.createElement("div");
    list.className = "review-findings";
    if (!review.findings.length) {
      const clean = document.createElement("div");
      clean.className = "review-clean";
      clean.textContent = "The graph has connected, titled nodes and no duplicate or invalid edges.";
      list.appendChild(clean);
    }
    for (const finding of review.findings) {
      const item = document.createElement("article");
      item.className = `review-finding review-${finding.severity}`;
      const copy = document.createElement("div");
      const label = document.createElement("strong");
      label.textContent = finding.code.replaceAll("_", " ");
      const message = document.createElement("p");
      message.textContent = finding.message;
      copy.append(label, message);
      item.appendChild(copy);
      if (finding.target) {
        const focus = mobileBarButton("Focus");
        focus.onclick = () => {
          if (focusReviewTarget(finding.target)) overlay.closeDialog();
        };
        item.appendChild(focus);
      }
      list.appendChild(item);
    }

    dialog.append(head, summary, list);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);
    const close = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = close;
    closeButton.onclick = close;
    overlay.onclick = (evt) => { if (evt.target === overlay) close(); };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        close();
        return;
      }
      trapDialogFocus(evt, dialog);
    });
    closeButton.focus();
  }

  const SHORTCUTS = [
    ["Ctrl / Cmd + K", "Open command palette"],
    ["/", "Find and focus a node"],
    ["Alt + O", "Open the accessible diagram outline"],
    ["F2 / Enter", "Edit selected node or edge label"],
    ["Shift + 2", "Frame the current selection"],
    ["Ctrl / Cmd + S", "Save diagram"],
    ["Ctrl / Cmd + Z", "Undo"],
    ["Ctrl / Cmd + Y", "Redo"],
    ["Ctrl / Cmd + C", "Copy selection"],
    ["Ctrl / Cmd + X", "Cut selected nodes"],
    ["Ctrl / Cmd + V", "Paste"],
    ["Ctrl / Cmd + D", "Duplicate selected nodes"],
    ["Ctrl / Cmd + A", "Select all nodes"],
    ["Delete / Backspace", "Delete selection"],
    ["Escape", "Clear selection, close panels"],
    ["Shift + click", "Add to selection"],
    ["Drag on empty canvas", "Rubber-band select"],
    ["Shift + drag", "Rubber-band adds to selection"],
    ["Drag a corner handle", "Resize the selected node"],
    ["Arrow keys", "Move selected nodes by one grid step"],
    ["Shift + Arrow", "Move selected nodes by five grid steps"],
    ["Long press (touch)", "Add to selection"],
    ["Middle drag / Pan tool", "Pan the canvas"],
    ["Wheel / pinch", "Zoom"],
    ["?", "Show this help"]
  ];

  function toggleShortcuts() {
    const existing = document.querySelector(".shortcuts-overlay");
    if (existing) {
      closeDialog(".shortcuts-overlay");
      return;
    }
    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "shortcuts-overlay";
    overlay.setAttribute("role", "dialog");
    overlay.setAttribute("aria-modal", "true");
    overlay.setAttribute("aria-label", "Keyboard shortcuts");

    const card = document.createElement("div");
    card.className = "shortcuts-card";
    const head = document.createElement("div");
    head.className = "panel-head";
    const title = document.createElement("h2");
    title.textContent = "Shortcuts";
    const close = document.createElement("button");
    close.type = "button";
    close.textContent = "Close";
    head.append(title, close);
    card.appendChild(head);

    const table = document.createElement("dl");
    table.className = "shortcut-list";
    for (const [keys, what] of SHORTCUTS) {
      const dt = document.createElement("dt");
      dt.textContent = keys;
      const dd = document.createElement("dd");
      dd.textContent = what;
      table.append(dt, dd);
    }
    card.appendChild(table);
    overlay.appendChild(card);
    const closeShortcuts = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = closeShortcuts;
    close.onclick = closeShortcuts;
    overlay.onclick = (evt) => { if (evt.target === overlay) closeShortcuts(); };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closeShortcuts();
        return;
      }
      trapDialogFocus(evt, card);
    });
    document.body.appendChild(overlay);
    close.focus();
  }

  // ---------------------------------------------------------- version diff --
  const NODE_DIFF_FIELDS = ["title", "type", "x", "y", "width", "height", "color"];
  const EDGE_DIFF_FIELDS = ["source", "target", "label", "directed", "color"];

  // Coordinates round-trip through PostgreSQL doubles and JSON. That is exact
  // for the values the editor produces today, but alignment snapping already
  // yields fractional positions, so comparison is rounded to two decimals:
  // finer than any pointer can express, coarser than float formatting noise.
  function diffValue(value) {
    if (typeof value === "number") return String(Math.round(value * 100) / 100);
    if (value === undefined || value === null) return "";
    return String(value);
  }

  function describeNode(node) {
    return node.title ? `"${shortText(node.title, 28)}"` : node.key;
  }

  function describeEdge(edge) {
    const label = edge.label ? ` "${shortText(edge.label, 20)}"` : "";
    return `${edge.source} to ${edge.target}${label}`;
  }

  function diffCollection(before, after, fields, describe) {
    const index = (list) => new Map((list || []).map((item) => [item.key, item]));
    const old = index(before);
    const now = index(after);
    const rows = [];
    for (const [key, item] of now) {
      const previous = old.get(key);
      if (!previous) {
        rows.push({ kind: "added", text: `Added ${describe(item)}` });
        continue;
      }
      const changed = fields.filter((f) => diffValue(previous[f]) !== diffValue(item[f]));
      if (changed.length) {
        rows.push({ kind: "changed", text: `Changed ${describe(item)}: ${changed.join(", ")}` });
      }
    }
    for (const [key, item] of old) {
      if (!now.has(key)) rows.push({ kind: "removed", text: `Removed ${describe(item)}` });
    }
    return rows;
  }

  function diffDiagrams(before, after) {
    const rows = [];
    if (diffValue(before.title) !== diffValue(after.title)) {
      rows.push({ kind: "changed", text: `Title: "${before.title}" to "${after.title}"` });
    }
    if (diffValue(before.description) !== diffValue(after.description)) {
      rows.push({ kind: "changed", text: "Description changed" });
    }
    return rows
      .concat(diffCollection(before.nodes, after.nodes, NODE_DIFF_FIELDS, describeNode))
      .concat(diffCollection(before.edges, after.edges, EDGE_DIFF_FIELDS, describeEdge));
  }

  function showDiff(version, rows) {
    closeDialog(".shortcuts-overlay");
    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "shortcuts-overlay";
    overlay.setAttribute("role", "dialog");
    overlay.setAttribute("aria-modal", "true");
    overlay.setAttribute("aria-label", `Changes since version ${version.version_number}`);

    const card = document.createElement("div");
    card.className = "shortcuts-card";
    const head = document.createElement("div");
    head.className = "panel-head";
    const title = document.createElement("h2");
    title.textContent = `Since v${version.version_number}`;
    const close = document.createElement("button");
    close.type = "button";
    close.textContent = "Close";
    head.append(title, close);
    card.appendChild(head);

    // The editor buffer, not the saved diagram: comparing against the server
    // copy would hide exactly the unsaved work the user is trying to review.
    const note = document.createElement("p");
    note.className = "muted";
    note.textContent = rows.length
      ? `${rows.length} change${rows.length === 1 ? "" : "s"} between that snapshot and what is open now.`
      : "That snapshot matches what is open now.";
    card.appendChild(note);

    const list = document.createElement("ul");
    list.className = "diff-list";
    for (const row of rows) {
      const item = document.createElement("li");
      item.className = `diff-${row.kind}`;
      item.textContent = row.text;
      list.appendChild(item);
    }
    card.appendChild(list);
    overlay.appendChild(card);
    const closeDiff = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = closeDiff;
    close.onclick = closeDiff;
    overlay.onclick = (evt) => { if (evt.target === overlay) closeDiff(); };
    overlay.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closeDiff();
        return;
      }
      trapDialogFocus(evt, card);
    });
    document.body.appendChild(overlay);
    close.focus();
  }

  function cloneDiagram() {
    return JSON.parse(JSON.stringify(state.current));
  }

  let keyboardNudgeSignature = null;
  let keyboardNudgeTimer = null;

  function endKeyboardNudge() {
    window.clearTimeout(keyboardNudgeTimer);
    keyboardNudgeTimer = null;
    keyboardNudgeSignature = null;
  }

  function pushHistory(continueKeyboardNudge = false) {
    if (!state.current || state.readOnly) return;
    if (!continueKeyboardNudge) endKeyboardNudge();
    state.history.push(cloneDiagram());
    if (state.history.length > 80) state.history.shift();
    state.future = [];
    markDirty();
  }

  function draftKey(id) {
    return `nuigraph:draft:${id}`;
  }

  function setDraftStatus(text, dirty = state.dirty) {
    state.dirty = dirty;
    const status = el("draft-status");
    if (status) status.textContent = text;
    document.body.classList.toggle("dirty", dirty);
    refreshMobileActionBar();
  }

  function markDirty() {
    if (!state.current || state.readOnly) return;
    state.dirty = true;
    setDraftStatus("Drafting", true);
    queueDraftSave();
  }

  function queueDraftSave() {
    window.clearTimeout(state.draftTimer);
    state.draftTimer = window.setTimeout(saveDraftNow, 700);
  }

  function saveDraftNow() {
    if (!state.current || state.readOnly || !state.current.id) return;
    try {
      const payload = JSON.stringify({ saved_at: new Date().toISOString(), diagram: state.current });
      if (payload.length > 900000) {
        setDraftStatus("Draft too large", true);
        return;
      }
      localStorage.setItem(draftKey(state.current.id), payload);
      setDraftStatus("Draft saved", true);
    } catch {
      setDraftStatus("Draft blocked", true);
    }
  }

  function clearDraft(id) {
    if (id) localStorage.removeItem(draftKey(id));
    state.dirty = false;
    setDraftStatus("Saved", false);
  }

  function maybeRestoreDraft() {
    if (!state.current || state.readOnly || !state.current.id) {
      setDraftStatus(state.readOnly ? "Read-only" : "Saved", false);
      return;
    }
    try {
      const raw = localStorage.getItem(draftKey(state.current.id));
      if (!raw) {
        clearDraft(null);
        return;
      }
      const draft = JSON.parse(raw);
      const draftTime = Date.parse(draft.saved_at || "");
      const serverTime = Date.parse(state.current.updated_at || state.current.created_at || "");
      if (draft.diagram && draftTime && (!serverTime || draftTime > serverTime + 1000) && confirm("Restore unsaved browser draft for this diagram?")) {
        const serverMeta = {
          id: state.current.id,
          slug: state.current.slug,
          created_at: state.current.created_at,
          updated_at: state.current.updated_at,
          revision: state.current.revision,
          can_edit: state.current.can_edit,
          sharing_enabled: state.current.sharing_enabled,
          share_expires_at: state.current.share_expires_at,
          share_expired: state.current.share_expired,
          share_url: state.current.share_url
        };
        state.current = { ...draft.diagram, ...serverMeta };
        state.dirty = true;
        setDraftStatus("Draft restored", true);
        return;
      }
      setDraftStatus("Saved", false);
    } catch {
      setDraftStatus("Saved", false);
    }
  }

  function restoreSnapshot(snapshot) {
    const id = state.current?.id;
    state.current = snapshot;
    if (id && !state.current.id) state.current.id = id;
    el("diagram-title").value = state.current.title;
    state.selected = null;
    render();
  }

  function undo() {
    endKeyboardNudge();
    if (!state.history.length || !state.current) return;
    state.future.push(cloneDiagram());
    restoreSnapshot(state.history.pop());
    markDirty();
  }

  function redo() {
    endKeyboardNudge();
    if (!state.future.length || !state.current) return;
    state.history.push(cloneDiagram());
    restoreSnapshot(state.future.pop());
    markDirty();
  }

  function sharedSlugFromPath() {
    const match = location.pathname.match(/^\/d\/([A-Za-z0-9-]+)$/);
    return match ? match[1] : null;
  }

  function setReadOnly(readOnly) {
    state.readOnly = readOnly;
    document.body.classList.toggle("read-only", readOnly);
    ["save-diagram", "delete-selected", "make-version", "import-json", "auto-layout", "paste-selected", "export-json"].forEach((id) => {
      const node = el(id);
      if (node) node.disabled = readOnly;
    });
    refreshMobileActionBar();
  }

  function worldPoint(evt) {
    const rect = canvas.getBoundingClientRect();
    return {
      x: (evt.clientX - rect.left - state.pan.x) / state.zoom,
      y: (evt.clientY - rect.top - state.pan.y) / state.zoom
    };
  }

  const MIN_ZOOM = 0.25;
  const MAX_ZOOM = 2.5;

  function clampZoom(value) {
    return Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, value));
  }

  // Zoom about a fixed screen point so the content under the fingers (or the
  // cursor) stays put instead of sliding toward the origin.
  function zoomAround(clientX, clientY, nextZoom) {
    const rect = canvas.getBoundingClientRect();
    const zoom = clampZoom(nextZoom);
    const worldX = (clientX - rect.left - state.pan.x) / state.zoom;
    const worldY = (clientY - rect.top - state.pan.y) / state.zoom;
    state.zoom = zoom;
    state.pan.x = clientX - rect.left - worldX * zoom;
    state.pan.y = clientY - rect.top - worldY * zoom;
  }

  function frameNodes(nodes, maximumZoom) {
    if (!nodes.length) return;
    const rect = canvas.getBoundingClientRect();
    const minX = Math.min(...nodes.map((node) => node.x));
    const minY = Math.min(...nodes.map((node) => node.y));
    const maxX = Math.max(...nodes.map((node) => node.x + node.width));
    const maxY = Math.max(...nodes.map((node) => node.y + node.height));
    const width = Math.max(1, maxX - minX);
    const height = Math.max(1, maxY - minY);
    const mobile = window.matchMedia("(max-width: 760px)").matches;
    const horizontalPadding = mobile ? 28 : 64;
    const topPadding = mobile ? 28 : 64;
    const bottomPadding = mobile ? 84 : 64;
    const availableWidth = Math.max(1, rect.width - horizontalPadding * 2);
    const availableHeight = Math.max(1, rect.height - topPadding - bottomPadding);
    state.zoom = Math.min(maximumZoom, clampZoom(Math.min(availableWidth / width, availableHeight / height)));
    state.pan.x = horizontalPadding + (availableWidth - width * state.zoom) / 2 - minX * state.zoom;
    state.pan.y = topPadding + (availableHeight - height * state.zoom) / 2 - minY * state.zoom;
    render();
  }

  function zoomToFit() {
    frameNodes(state.current?.nodes || [], 1);
  }

  function selectedFrameNodes() {
    if (!state.current || !state.selected) return [];
    let keys = selectedNodeKeys();
    if (state.selected.type === "edge") {
      const edge = state.current.edges.find((candidate) => candidate.key === state.selected.key);
      keys = edge ? [edge.source, edge.target] : [];
    }
    const selected = new Set(keys);
    return state.current.nodes.filter((node) => selected.has(node.key));
  }

  function frameSelection() {
    frameNodes(selectedFrameNodes(), 1.75);
  }

  const LONG_PRESS_MS = 450;
  // Screen pixels of pointer travel before an empty-canvas drag counts as a
  // rubber-band rather than a click.
  const MARQUEE_MIN_DRAG = 4;

  function cancelLongPress() {
    window.clearTimeout(state.longPressTimer);
    state.longPressTimer = null;
  }

  // Touch has no modifier keys, so holding a node for a moment arms additive
  // selection and a short buzz (where supported) confirms it.
  function armLongPress(onFire) {
    cancelLongPress();
    state.longPressTimer = window.setTimeout(() => {
      state.multiSelectArmed = true;
      if (navigator.vibrate) navigator.vibrate(15);
      onFire();
    }, LONG_PRESS_MS);
  }

  function pointerDistance(a, b) {
    return Math.hypot(a.x - b.x, a.y - b.y);
  }

  function activePointerPair() {
    const points = [...state.pointers.values()];
    return points.length === 2 ? points : null;
  }

  // A second finger always wins: abandon any in-progress drag or pan so the
  // gesture cannot both move a node and scale the canvas.
  function beginPinch() {
    const pair = activePointerPair();
    if (!pair) return;
    state.dragging = null;
    state.panning = null;
    state.pinch = {
      distance: pointerDistance(pair[0], pair[1]),
      zoom: state.zoom
    };
  }

  function updatePinch() {
    const pair = activePointerPair();
    if (!pair || !state.pinch || state.pinch.distance <= 0) return;
    const distance = pointerDistance(pair[0], pair[1]);
    const centerX = (pair[0].x + pair[1].x) / 2;
    const centerY = (pair[0].y + pair[1].y) / 2;
    zoomAround(centerX, centerY, state.pinch.zoom * (distance / state.pinch.distance));
    render();
  }

  function viewportCenterPoint() {
    const rect = canvas.getBoundingClientRect();
    return {
      x: (rect.width / 2 - state.pan.x) / state.zoom,
      y: (rect.height / 2 - state.pan.y) / state.zoom
    };
  }

  function nodeCenter(node) {
    return { x: node.x + node.width / 2, y: node.y + node.height / 2 };
  }

  function snapValue(value, grid = state.gridSize) {
    return state.snap ? Math.round(value / grid) * grid : value;
  }

  function applyGridSize(value, persist = true) {
    const size = Number(value);
    if (!GRID_SIZES.includes(size)) return false;
    state.gridSize = size;
    const pattern = el("grid");
    pattern.setAttribute("width", String(size));
    pattern.setAttribute("height", String(size));
    pattern.querySelector("path")?.setAttribute("d", `M ${size} 0 L 0 0 0 ${size}`);
    el("grid-size").value = String(size);
    if (persist) {
      try {
        localStorage.setItem(GRID_SIZE_STORAGE, String(size));
      } catch {
        // Storage can be disabled; the in-memory grid still works.
      }
    }
    return true;
  }

  function initGridSize() {
    let saved = null;
    try {
      saved = localStorage.getItem(GRID_SIZE_STORAGE);
    } catch {
      // Keep the documented default when storage is unavailable.
    }
    if (!applyGridSize(saved === null ? state.gridSize : saved, false)) {
      applyGridSize(16, false);
      try {
        localStorage.removeItem(GRID_SIZE_STORAGE);
      } catch {
        // The valid in-memory fallback is already active.
      }
    }
  }

  function cycleGridSize() {
    const index = GRID_SIZES.indexOf(state.gridSize);
    const next = GRID_SIZES[(index + 1) % GRID_SIZES.length];
    applyGridSize(next);
    toast(`Grid spacing set to ${next}px.`);
  }

  function clamp(value, low, high) {
    return Math.max(low, Math.min(high, value));
  }

  // The same bounds DiagramValidator clamps to. Enforcing them while dragging
  // means the handle stops where the node stops instead of the server silently
  // rewriting the size on the next save.
  const NODE_MIN_WIDTH = 72;
  const NODE_MAX_WIDTH = 600;
  const NODE_MIN_HEIGHT = 48;
  const NODE_MAX_HEIGHT = 400;

  function nodeGeometryLocked(node) {
    return node?.metadata?.geometry_locked === true;
  }

  function selectedNodeKeys() {
    if (!state.selected) return [];
    if (state.selected.type === "node") return [state.selected.key];
    if (state.selected.type === "nodes") return state.selected.keys;
    return [];
  }

  let lastSelectionAnnouncement = null;

  function selectionAnnouncement() {
    if (!state.current) return "No diagram is open.";
    const nodeCount = state.current.nodes?.length || 0;
    const edgeCount = state.current.edges?.length || 0;
    if (!state.selected) {
      return `Selection cleared. Diagram has ${nodeCount} node${nodeCount === 1 ? "" : "s"} and ${edgeCount} edge${edgeCount === 1 ? "" : "s"}.`;
    }
    if (state.selected.type === "node") {
      const node = state.current.nodes.find((candidate) => candidate.key === state.selected.key);
      return node
        ? `Node selected: ${node.title || "Untitled node"}, type ${node.type || "node"}${nodeGeometryLocked(node) ? ", geometry locked" : ""}.`
        : "The selected node is unavailable.";
    }
    if (state.selected.type === "nodes") {
      const selected = new Set(state.selected.keys);
      const nodes = state.current.nodes.filter((node) => selected.has(node.key));
      const locked = nodes.filter(nodeGeometryLocked).length;
      return `${nodes.length} nodes selected${locked ? `, ${locked} geometry locked` : ""}.`;
    }
    if (state.selected.type === "edge") {
      const edge = state.current.edges.find((candidate) => candidate.key === state.selected.key);
      if (!edge) return "The selected edge is unavailable.";
      const source = state.current.nodes.find((node) => node.key === edge.source);
      const target = state.current.nodes.find((node) => node.key === edge.target);
      const sourceName = source?.title || edge.source || "unknown node";
      const targetName = target?.title || edge.target || "unknown node";
      const relationship = edge.directed
        ? `from ${sourceName} to ${targetName}`
        : `between ${sourceName} and ${targetName}`;
      return `Edge selected: ${edge.label || "Unlabelled edge"}, ${relationship}.`;
    }
    return "Diagram selection changed.";
  }

  function updateSelectionAnnouncement() {
    const message = selectionAnnouncement();
    if (message === lastSelectionAnnouncement) return;
    lastSelectionAnnouncement = message;
    editorLiveStatus.textContent = message;
  }

  let inlineLabelEditor = null;

  function labelTarget(type, key) {
    if (!state.current || !key) return null;
    if (type === "node") {
      const item = state.current.nodes.find((node) => node.key === key);
      return item ? {
        type, key, item, value: item.title, required: true,
        x: item.x + item.width / 2,
        y: item.y + item.height / 2
      } : null;
    }
    if (type === "edge") {
      const item = state.current.edges.find((edge) => edge.key === key);
      if (!item) return null;
      const source = state.current.nodes.find((node) => node.key === item.source);
      const target = state.current.nodes.find((node) => node.key === item.target);
      if (!source || !target) return null;
      const a = nodeCenter(source);
      const b = nodeCenter(target);
      return {
        type, key, item, value: item.label || "", required: false,
        x: (a.x + b.x) / 2,
        y: (a.y + b.y) / 2 - 10
      };
    }
    return null;
  }

  function editableSelectedLabelTarget() {
    if (!state.selected || !["node", "edge"].includes(state.selected.type)) return null;
    return labelTarget(state.selected.type, state.selected.key);
  }

  function closeInlineLabelEditor(restoreFocus = false) {
    if (!inlineLabelEditor) return;
    const { element, returnFocus } = inlineLabelEditor;
    inlineLabelEditor = null;
    element.remove();
    if (restoreFocus) {
      const target = returnFocus?.isConnected ? returnFocus : canvas;
      target.focus?.({ preventScroll: true });
    }
  }

  function positionInlineLabelEditor() {
    if (!inlineLabelEditor) return;
    const target = labelTarget(inlineLabelEditor.type, inlineLabelEditor.key);
    const wrap = canvas.closest(".canvas-wrap");
    if (!target || !wrap || state.readOnly || state.current !== inlineLabelEditor.diagram) {
      closeInlineLabelEditor();
      return;
    }

    const { element } = inlineLabelEditor;
    const width = element.offsetWidth || 320;
    const height = element.offsetHeight || 120;
    const halfWidth = Math.min(width / 2, Math.max(0, wrap.clientWidth / 2 - 8));
    const halfHeight = Math.min(height / 2, Math.max(0, wrap.clientHeight / 2 - 8));
    const desiredLeft = state.pan.x + target.x * state.zoom;
    const desiredTop = state.pan.y + target.y * state.zoom;
    element.style.left = `${clamp(desiredLeft, halfWidth + 8, wrap.clientWidth - halfWidth - 8)}px`;
    element.style.top = `${clamp(desiredTop, halfHeight + 8, wrap.clientHeight - halfHeight - 8)}px`;
  }

  function openInlineLabelEditor(type = state.selected?.type, key = state.selected?.key) {
    if (!state.current || state.readOnly) return;
    const target = labelTarget(type, key);
    if (!target) return;
    if (inlineLabelEditor?.type === type && inlineLabelEditor?.key === key) {
      inlineLabelEditor.input.focus();
      inlineLabelEditor.input.select();
      return;
    }

    closeInlineLabelEditor();
    state.selected = { type, key };
    render();

    const returnFocus = document.activeElement === document.body ? canvas : document.activeElement;
    const form = document.createElement("form");
    form.className = "inline-label-editor";
    form.setAttribute("role", "dialog");
    form.setAttribute("aria-label", `Edit ${type} label`);

    const label = document.createElement("label");
    label.htmlFor = "inline-label-editor-input";
    label.textContent = `${type === "node" ? "Node" : "Edge"} label`;
    const input = document.createElement("input");
    input.id = "inline-label-editor-input";
    input.type = "text";
    input.maxLength = 160;
    input.required = target.required;
    input.autocomplete = "off";
    input.value = target.value;
    input.setAttribute("aria-label", label.textContent);
    input.addEventListener("input", () => input.setCustomValidity(""));

    const actions = document.createElement("div");
    actions.className = "inline-label-editor-actions";
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "Cancel";
    const save = document.createElement("button");
    save.type = "submit";
    save.className = "primary";
    save.textContent = "Save";
    actions.append(cancel, save);
    form.append(label, input, actions);
    canvas.closest(".canvas-wrap")?.appendChild(form);

    inlineLabelEditor = { element: form, input, type, key, returnFocus, diagram: state.current };
    positionInlineLabelEditor();
    input.focus();
    input.select();

    cancel.onclick = () => closeInlineLabelEditor(true);
    form.addEventListener("keydown", (evt) => {
      if (evt.key === "Escape") {
        evt.preventDefault();
        evt.stopPropagation();
        closeInlineLabelEditor(true);
        return;
      }
      trapDialogFocus(evt, form);
    });
    form.addEventListener("submit", (evt) => {
      evt.preventDefault();
      const nextValue = input.value.trim();
      if (target.required && !nextValue) {
        input.setCustomValidity("Enter a node label.");
        input.reportValidity();
        return;
      }
      if (nextValue.length > 160) {
        input.setCustomValidity("Use no more than 160 characters.");
        input.reportValidity();
        return;
      }
      const current = labelTarget(type, key);
      if (!current || state.current !== inlineLabelEditor?.diagram) {
        closeInlineLabelEditor(true);
        return;
      }
      if (nextValue === current.value) {
        closeInlineLabelEditor(true);
        return;
      }
      pushHistory();
      if (type === "node") current.item.title = nextValue;
      else current.item.label = nextValue;
      markDirty();
      closeInlineLabelEditor(true);
      render();
      toast(`${type === "node" ? "Node" : "Edge"} label updated.`);
    });
  }

  function canArrangeSelection(minimum) {
    return !state.readOnly && selectedNodeKeys().length >= minimum && movableSelectedNodeKeys().length > 0;
  }

  function arrangeSelection(mode) {
    if (!state.current || state.readOnly) return;
    const arrangeNodes = globalThis.NuiGraphArrange?.arrangeNodes;
    if (typeof arrangeNodes !== "function") {
      toast("Selection arrangement is unavailable; reload the editor assets.", "error");
      return;
    }
    const locked = new Set(state.current.nodes.filter(nodeGeometryLocked).map((node) => node.key));
    const updates = arrangeNodes(state.current.nodes, selectedNodeKeys(), mode)
      .filter((update) => typeof update?.key === "string" &&
        Number.isFinite(update.x) && Number.isFinite(update.y) && !locked.has(update.key));
    if (!updates.length) {
      toast("The selected nodes are already arranged.");
      return;
    }
    pushHistory();
    const byKey = new Map(updates.map((update) => [update.key, update]));
    for (const node of state.current.nodes) {
      const update = byKey.get(node.key);
      if (!update) continue;
      node.x = update.x;
      node.y = update.y;
    }
    markDirty();
    render();
    toast(`${updates.length} node${updates.length === 1 ? "" : "s"} arranged.`);
  }

  function nudgeSelectedNodes(deltaX, deltaY) {
    if (!state.current || state.readOnly) return;
    const keys = movableSelectedNodeKeys();
    if (!keys.length) return;
    const selected = new Set(keys);
    const nodes = state.current.nodes.filter((node) => selected.has(node.key));
    if (!nodes.length) return;

    const boundedX = clamp(
      deltaX,
      Math.max(...nodes.map((node) => -100000 - node.x)),
      Math.min(...nodes.map((node) => 100000 - node.x))
    );
    const boundedY = clamp(
      deltaY,
      Math.max(...nodes.map((node) => -100000 - node.y)),
      Math.min(...nodes.map((node) => 100000 - node.y))
    );
    if (boundedX === 0 && boundedY === 0) return;

    const signature = `${state.current.id || "new"}:${keys.slice().sort().join("|")}`;
    if (keyboardNudgeSignature !== signature) {
      pushHistory(true);
      keyboardNudgeSignature = signature;
    }
    for (const node of nodes) {
      node.x += boundedX;
      node.y += boundedY;
    }
    markDirty();
    render();
    window.clearTimeout(keyboardNudgeTimer);
    keyboardNudgeTimer = window.setTimeout(endKeyboardNudge, 400);
  }

  function isNodeSelected(key) {
    return selectedNodeKeys().includes(key);
  }

  // One node and many nodes are different selection shapes, so every path that
  // produces a set of keys goes through here rather than guessing the shape.
  function selectNodeKeys(keys) {
    state.selected = keys.length === 0 ? null
      : keys.length === 1 ? { type: "node", key: keys[0] }
      : { type: "nodes", keys };
  }

  function toggleNodeSelection(key) {
    const keys = new Set(selectedNodeKeys());
    if (keys.has(key)) {
      keys.delete(key);
    } else {
      keys.add(key);
    }
    selectNodeKeys([...keys]);
  }

  function selectedNodeObjects() {
    if (!state.current) return [];
    const selected = new Set(selectedNodeKeys());
    return state.current.nodes.filter((node) => selected.has(node.key));
  }

  function movableSelectedNodeKeys() {
    return selectedNodeObjects().filter((node) => !nodeGeometryLocked(node)).map((node) => node.key);
  }

  function toggleSelectedGeometryLock() {
    if (!state.current || state.readOnly) return;
    const nodes = selectedNodeObjects();
    if (!nodes.length) return;
    const lock = nodes.some((node) => !nodeGeometryLocked(node));
    pushHistory();
    for (const node of nodes) {
      const metadata = node.metadata && typeof node.metadata === "object" && !Array.isArray(node.metadata)
        ? { ...node.metadata }
        : {};
      if (lock) metadata.geometry_locked = true;
      else delete metadata.geometry_locked;
      node.metadata = metadata;
    }
    state.dragging = null;
    state.resizing = null;
    state.guides = [];
    render();
    toast(`${nodes.length} node${nodes.length === 1 ? "" : "s"} ${lock ? "locked" : "unlocked"}.`);
  }

  function selectAllNodes() {
    const keys = state.current?.nodes?.map((node) => node.key) || [];
    if (!keys.length) return;
    selectNodeKeys(keys);
    state.edgeSource = null;
    setTool("select");
    render();
    canvas.focus({ preventScroll: true });
  }

  function applyGraphSelection(method, description) {
    if (!state.current) return;
    const selectKeys = globalThis.NuiGraphSelection?.[method];
    if (typeof selectKeys !== "function") {
      toast("Graph selection is unavailable; reload the editor assets.", "error");
      return;
    }
    const keys = selectKeys(state.current, selectedNodeKeys());
    if (!keys.length) return;
    selectNodeKeys(keys);
    state.edgeSource = null;
    setTool("select");
    render();
    canvas.focus({ preventScroll: true });
    toast(`${keys.length} ${description} node${keys.length === 1 ? "" : "s"} selected.`);
  }

  function selectNeighbourNodes() {
    applyGraphSelection("neighbourNodeKeys", "neighboring");
  }

  function selectConnectedNodes() {
    applyGraphSelection("connectedNodeKeys", "connected");
  }

  function selectTerminalNodes(mode) {
    if (!state.current) return;
    const terminalNodeKeys = globalThis.NuiGraphSelection?.terminalNodeKeys;
    if (typeof terminalNodeKeys !== "function") {
      toast("Graph selection is unavailable; reload the editor assets.", "error");
      return;
    }
    const keys = terminalNodeKeys(state.current, mode);
    selectNodeKeys(keys);
    state.edgeSource = null;
    setTool("select");
    render();
    canvas.focus({ preventScroll: true });
    toast(keys.length
      ? `${keys.length} ${mode} node${keys.length === 1 ? "" : "s"} selected.`
      : `No ${mode} nodes found.`);
  }

  // A rubber-band drag can go up and to the left, so the stored corners are the
  // start and the current pointer, not a top-left plus a size.
  function marqueeRect(m) {
    return {
      x: Math.min(m.x0, m.x1),
      y: Math.min(m.y0, m.y1),
      width: Math.abs(m.x1 - m.x0),
      height: Math.abs(m.y1 - m.y0)
    };
  }

  // Touching the band is enough. Requiring full containment means a band drawn
  // across a wide row of nodes catches none of them, which reads as broken.
  function nodeKeysInRect(rect) {
    if (!state.current) return [];
    return state.current.nodes
      .filter((n) => n.x < rect.x + rect.width && n.x + n.width > rect.x &&
                     n.y < rect.y + rect.height && n.y + n.height > rect.y)
      .map((n) => n.key);
  }

  function applyMarqueeSelection(m) {
    const inside = nodeKeysInRect(marqueeRect(m));
    // Shift extends what was already selected, so the band is re-applied to the
    // original set every move instead of accumulating as the pointer wanders.
    selectNodeKeys(m.additive ? [...new Set(m.baseKeys.concat(inside))] : inside);
  }

  // Handles are placed by sign rather than by name: -1 is the left/top edge, 1
  // the right/bottom, 0 the midpoint, which is also exactly the arithmetic the
  // resize itself needs.
  const HANDLE_SIGNS = [[-1, -1], [0, -1], [1, -1], [-1, 0], [1, 0], [-1, 1], [0, 1], [1, 1]];
  const HANDLE_CURSOR = {
    "-1,-1": "nwse", "1,1": "nwse", "1,-1": "nesw", "-1,1": "nesw",
    "0,-1": "ns", "0,1": "ns", "-1,0": "ew", "1,0": "ew"
  };
  const HANDLE_SIZE = 9;
  const HANDLE_TOUCH_SIZE = 26;

  // Resize is a single-node affair: with several selected there is no one box
  // to put the handles on.
  function resizeTarget() {
    if (state.readOnly || !state.current) return null;
    if (!state.selected || state.selected.type !== "node") return null;
    const node = state.current.nodes.find((n) => n.key === state.selected.key) || null;
    return nodeGeometryLocked(node) ? null : node;
  }

  function applyResize(node, gesture, point) {
    const o = gesture.orig;
    if (gesture.sx !== 0) {
      const edge = snapValue(point.x);
      const width = clamp(gesture.sx > 0 ? edge - o.x : o.x + o.width - edge, NODE_MIN_WIDTH, NODE_MAX_WIDTH);
      // x comes from the clamped width, not from the pointer: once the node hits
      // its minimum the anchored edge has to stay put instead of being dragged
      // along by the handle the user is still moving.
      node.x = gesture.sx > 0 ? o.x : o.x + o.width - width;
      node.width = width;
    }
    if (gesture.sy !== 0) {
      const edge = snapValue(point.y);
      const height = clamp(gesture.sy > 0 ? edge - o.y : o.y + o.height - edge, NODE_MIN_HEIGHT, NODE_MAX_HEIGHT);
      node.y = gesture.sy > 0 ? o.y : o.y + o.height - height;
      node.height = height;
    }
  }

  // Screen pixels within which a dragged box latches onto another node's edge
  // or centre line.
  const ALIGN_THRESHOLD = 6;
  const GUIDE_OVERHANG = 14;

  // Three lines per axis per node: both edges and the centre. Aligning centres
  // is what makes a row of differently sized nodes line up sensibly.
  const alignLinesX = (r) => [r.x, r.x + r.width / 2, r.x + r.width];
  const alignLinesY = (r) => [r.y, r.y + r.height / 2, r.y + r.height];

  function movingBox(dragging, deltaX, deltaY) {
    const keys = dragging.keys || [dragging.key];
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const key of keys) {
      const original = dragging.originals[key];
      const node = state.current.nodes.find((n) => n.key === key);
      if (!original || !node) continue;
      minX = Math.min(minX, original.x + deltaX);
      minY = Math.min(minY, original.y + deltaY);
      maxX = Math.max(maxX, original.x + deltaX + node.width);
      maxY = Math.max(maxY, original.y + deltaY + node.height);
    }
    return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
  }

  // Returns the nudge that puts the dragged box on the nearest alignment line,
  // plus which nodes it matched so the guide can be drawn across all of them.
  function alignmentAdjust(box, movingKeys) {
    const tolerance = ALIGN_THRESHOLD / state.zoom;
    const others = state.current.nodes.filter((n) => !movingKeys.includes(n.key));
    const result = { dx: 0, dy: 0, matches: [] };
    for (const axis of ["x", "y"]) {
      const lines = axis === "x" ? alignLinesX : alignLinesY;
      const mine = lines(box);
      let best = null;
      for (const other of others) {
        for (const value of lines(other)) {
          for (const own of mine) {
            const distance = Math.abs(value - own);
            // Strictly closer wins, so a tie between two equally near nodes
            // keeps the first and the guide does not flicker between them.
            if (distance <= tolerance && (!best || distance < best.distance)) {
              best = { distance, value, offset: value - own };
            }
          }
        }
      }
      if (!best) continue;
      if (axis === "x") result.dx = best.offset; else result.dy = best.offset;
      result.matches.push({
        axis,
        value: best.value,
        others: others.filter((n) => lines(n).includes(best.value))
      });
    }
    return result;
  }

  // The guide spans every box it touches, so it reads as a relationship rather
  // than as a line that happens to pass nearby.
  function guideSpans(matches, box) {
    return matches.map((match) => {
      const rects = match.others.concat([box]);
      if (match.axis === "x") {
        return {
          axis: "x",
          value: match.value,
          from: Math.min(...rects.map((r) => r.y)) - GUIDE_OVERHANG,
          to: Math.max(...rects.map((r) => r.y + r.height)) + GUIDE_OVERHANG
        };
      }
      return {
        axis: "y",
        value: match.value,
        from: Math.min(...rects.map((r) => r.x)) - GUIDE_OVERHANG,
        to: Math.max(...rects.map((r) => r.x + r.width)) + GUIDE_OVERHANG
      };
    });
  }

  function renderGuides() {
    for (const guide of state.guides) {
      const ends = guide.axis === "x"
        ? { x1: guide.value, y1: guide.from, x2: guide.value, y2: guide.to }
        : { x1: guide.from, y1: guide.value, x2: guide.to, y2: guide.value };
      svg("line", {
        ...ends,
        class: "align-guide",
        "stroke-width": 1 / state.zoom,
        "stroke-dasharray": `${6 / state.zoom} ${4 / state.zoom}`
      }, overlayLayer);
    }
  }

  function beginResize(evt, node, sx, sy) {
    if (state.readOnly || nodeGeometryLocked(node)) return;
    // Without this the canvas would also see the press and start a rubber-band
    // underneath the resize.
    evt.stopPropagation();
    state.pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
    if (state.pointers.size > 1) return;
    pushHistory();
    state.resizing = {
      key: node.key,
      sx,
      sy,
      orig: { x: node.x, y: node.y, width: node.width, height: node.height }
    };
    canvas.setPointerCapture(evt.pointerId);
  }

  function renderResizeHandles() {
    const node = resizeTarget();
    // Mid-band the selection is still changing, so handles would flicker across
    // whichever node happens to be alone in the box.
    if (!node || (state.marquee && state.marquee.moved)) return;
    const size = HANDLE_SIZE / state.zoom;
    const touch = HANDLE_TOUCH_SIZE / state.zoom;
    for (const [sx, sy] of HANDLE_SIGNS) {
      const cx = node.x + (node.width * (sx + 1)) / 2;
      const cy = node.y + (node.height * (sy + 1)) / 2;
      const cursor = HANDLE_CURSOR[`${sx},${sy}`];
      const box = (side, cls) => svg("rect", {
        class: cls, x: cx - side / 2, y: cy - side / 2, width: side, height: side
      }, overlayLayer);
      // Invisible fat square first so a fingertip can grab a 9px handle, same
      // trick the edges use.
      const hit = box(touch, `resize-hit resize-${cursor}`);
      const visible = box(size, `resize-handle resize-${cursor}`);
      visible.setAttribute("rx", size / 4);
      visible.setAttribute("stroke-width", 1.5 / state.zoom);
      hit.addEventListener("pointerdown", (evt) => beginResize(evt, node, sx, sy));
      visible.addEventListener("pointerdown", (evt) => beginResize(evt, node, sx, sy));
    }
  }

  function svg(tag, attrs = {}, parent) {
    const node = document.createElementNS(svgNS, tag);
    for (const [key, value] of Object.entries(attrs)) {
      node.setAttribute(key, String(value));
    }
    if (parent) parent.appendChild(node);
    return node;
  }

  function shortText(text, max = 22) {
    text = text || "";
    return text.length > max ? `${text.slice(0, max - 1)}...` : text;
  }

  function sampleDiagram() {
    return {
      title: "Sample Cloud Architecture",
      description: "Demo service graph for NuiGraph Studio.",
      nodes: [
        { key: "client", type: "external", title: "Client Apps", x: 40, y: 130, width: 170, height: 80, color: "#fb7185", metadata: {} },
        { key: "gateway", type: "api", title: "API Gateway", x: 300, y: 130, width: 180, height: 80, color: "#60a5fa", metadata: {} },
        { key: "auth", type: "service", title: "Auth Service", x: 570, y: 40, width: 180, height: 80, color: "#22c55e", metadata: {} },
        { key: "orders", type: "service", title: "Orders Service", x: 570, y: 170, width: 180, height: 80, color: "#22c55e", metadata: {} },
        { key: "queue", type: "process", title: "Event Queue", x: 830, y: 170, width: 170, height: 80, color: "#38bdf8", metadata: {} },
        { key: "db", type: "database", title: "PostgreSQL", x: 830, y: 40, width: 170, height: 90, color: "#a78bfa", metadata: {} },
        { key: "decision", type: "decision", title: "Fraud Check", x: 1090, y: 150, width: 160, height: 110, color: "#f59e0b", metadata: {} },
        { key: "note", type: "note", title: "Versioned diagrams stored in PostgreSQL", x: 1030, y: 20, width: 260, height: 90, color: "#fde047", metadata: {} }
      ],
      edges: [
        { key: "e_client_gateway", source: "client", target: "gateway", label: "HTTPS", directed: true, color: "#94a3b8", metadata: {} },
        { key: "e_gateway_auth", source: "gateway", target: "auth", label: "OIDC", directed: true, color: "#94a3b8", metadata: {} },
        { key: "e_gateway_orders", source: "gateway", target: "orders", label: "REST", directed: true, color: "#94a3b8", metadata: {} },
        { key: "e_auth_db", source: "auth", target: "db", label: "sessions", directed: true, color: "#a78bfa", metadata: {} },
        { key: "e_orders_db", source: "orders", target: "db", label: "writes", directed: true, color: "#a78bfa", metadata: {} },
        { key: "e_orders_queue", source: "orders", target: "queue", label: "events", directed: true, color: "#38bdf8", metadata: {} },
        { key: "e_queue_decision", source: "queue", target: "decision", label: "async", directed: true, color: "#f59e0b", metadata: {} }
      ]
    };
  }

  function downloadBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  function exportSvg() {
    if (!state.current) return;
    const clone = canvas.cloneNode(true);
    clone.setAttribute("xmlns", svgNS);
    clone.setAttribute("width", canvas.clientWidth || 1200);
    clone.setAttribute("height", canvas.clientHeight || 800);
    const css = `text{font-family:Inter,Arial,sans-serif}.node-title{fill:#071018;font-weight:700;font-size:14px;text-anchor:middle;dominant-baseline:middle}.node-subtitle{fill:rgba(7,16,24,.72);font-size:10px;text-anchor:middle;dominant-baseline:middle;text-transform:uppercase}.edge-label{fill:#dbeafe;paint-order:stroke;stroke:#0b0f14;stroke-width:4;font-size:12px;text-anchor:middle;dominant-baseline:middle}.edge-path{stroke-width:2.4;fill:none}`;
    const style = document.createElementNS(svgNS, "style");
    style.textContent = css;
    clone.insertBefore(style, clone.firstChild);
    const text = new XMLSerializer().serializeToString(clone);
    downloadBlob(new Blob([text], { type: "image/svg+xml" }), `${state.current.slug || state.current.title || "diagram"}.svg`);
  }

  function exportPng() {
    if (!state.current) return;
    const clone = canvas.cloneNode(true);
    clone.setAttribute("xmlns", svgNS);
    const width = canvas.clientWidth || 1200;
    const height = canvas.clientHeight || 800;
    clone.setAttribute("width", width);
    clone.setAttribute("height", height);
    const text = new XMLSerializer().serializeToString(clone);
    const img = new Image();
    const url = URL.createObjectURL(new Blob([text], { type: "image/svg+xml" }));
    img.onload = () => {
      const out = document.createElement("canvas");
      out.width = width * 2;
      out.height = height * 2;
      const ctx = out.getContext("2d");
      ctx.fillStyle = "#090d12";
      ctx.fillRect(0, 0, out.width, out.height);
      ctx.scale(2, 2);
      ctx.drawImage(img, 0, 0);
      URL.revokeObjectURL(url);
      out.toBlob((blob) => {
        if (blob) downloadBlob(blob, `${state.current.slug || state.current.title || "diagram"}.png`);
      }, "image/png");
    };
    img.src = url;
  }

  const MERMAID_SHAPES = {
    decision: (t) => `{${t}}`,
    database: (t) => `[(${t})]`,
    external: (t) => `[/${t}/]`,
    note: (t) => `>${t}]`,
    api: (t) => `([${t}])`,
    service: (t) => `[[${t}]]`
  };

  function mermaidLabel(text) {
    // Quotes and brackets would terminate the node shape early.
    return (text || "").replace(/["\[\]{}()<>|]/g, " ").trim() || "node";
  }

  function mermaidId(key) {
    return `n_${String(key).replace(/[^A-Za-z0-9_]/g, "_")}`;
  }

  function toMermaid(diagram) {
    const lines = ["flowchart LR"];
    for (const node of diagram.nodes) {
      const shape = MERMAID_SHAPES[node.type] || ((t) => `[${t}]`);
      lines.push(`  ${mermaidId(node.key)}${shape(mermaidLabel(node.title))}`);
    }
    for (const edge of diagram.edges) {
      const arrow = edge.directed ? "-->" : "---";
      const label = mermaidLabel(edge.label);
      const mid = edge.label ? `${arrow}|${label}|` : arrow;
      lines.push(`  ${mermaidId(edge.source)} ${mid} ${mermaidId(edge.target)}`);
    }
    return lines.join("\n");
  }

  function exportMermaid() {
    if (!state.current) return;
    const text = toMermaid(state.current);
    downloadBlob(new Blob([text], { type: "text/plain" }),
      `${state.current.slug || state.current.title || "diagram"}.mmd`);
    toast("Mermaid file exported.", "success");
  }

  async function copyShareLink(url) {
    if (!url) return false;
    try {
      await navigator.clipboard.writeText(url);
      toast("Share link copied.", "success");
      return true;
    } catch {
      prompt("Share link", url);
      return false;
    }
  }

  function shareDiagram() {
    if (!state.current) return;
    if (state.readOnly) {
      copyShareLink(state.current.share_url || location.href);
      return;
    }
    closeDialog(".command-palette-overlay");
    const previousFocus = document.activeElement;
    const overlay = document.createElement("div");
    overlay.className = "command-palette-overlay sharing-overlay";
    const dialog = document.createElement("section");
    dialog.className = "sharing-dialog";
    dialog.setAttribute("role", "dialog");
    dialog.setAttribute("aria-modal", "true");
    dialog.setAttribute("aria-labelledby", "sharing-title");

    const head = document.createElement("div");
    head.className = "command-palette-head";
    const title = document.createElement("h2");
    title.id = "sharing-title";
    title.textContent = "Share diagram";
    const closeButton = mobileBarButton("Close");
    head.append(title, closeButton);
    const content = document.createElement("div");
    content.className = "sharing-content";
    const liveStatus = document.createElement("div");
    liveStatus.className = "command-status";
    liveStatus.setAttribute("aria-live", "polite");
    dialog.append(head, content, liveStatus);
    overlay.appendChild(dialog);
    document.body.appendChild(overlay);

    const close = () => {
      overlay.remove();
      if (previousFocus?.isConnected) previousFocus.focus();
    };
    overlay.closeDialog = close;
    closeButton.onclick = close;
    overlay.onclick = (event) => { if (event.target === overlay) close(); };
    overlay.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        close();
        return;
      }
      trapDialogFocus(event, dialog);
    });

    const mergeSharingMetadata = (updated) => {
      if (!state.current || state.current.id !== updated.id) return;
      state.current.revision = updated.revision;
      state.current.updated_at = updated.updated_at;
      state.current.slug = updated.slug;
      state.current.sharing_enabled = updated.sharing_enabled;
      state.current.share_expires_at = updated.share_expires_at;
      state.current.share_expired = updated.share_expired;
      state.current.share_url = updated.share_url;
    };

    const changeSharing = async (action, expiresIn = undefined) => {
      const buttons = [...dialog.querySelectorAll("button")];
      buttons.forEach((button) => { button.disabled = true; });
      const progressLabel = action === "set_expiry"
        ? "Expiry update"
        : `${action[0].toUpperCase()}${action.slice(1)}`;
      liveStatus.textContent = `${progressLabel} in progress...`;
      try {
        const request = { action };
        if (expiresIn !== undefined) request.expires_in = expiresIn;
        const updated = await api(`/api/diagrams/${state.current.id}/sharing`, {
          method: "POST",
          headers: { "If-Match": diagramEtag(state.current) },
          body: JSON.stringify(request)
        });
        mergeSharingMetadata(updated);
        render();
        renderContent();
        loadDiagrams().catch(() => {});
        liveStatus.textContent = action === "disable"
          ? "Sharing disabled. The previous URL no longer works."
          : action === "rotate"
            ? "Link rotated. The previous URL no longer works."
            : action === "set_expiry"
              ? "Link expiry updated."
              : "Sharing enabled with a new link.";
      } catch (error) {
        if (!handleRevisionConflict(
          error,
          state.dirty,
          "A newer revision was saved elsewhere. Reopen the diagram before changing sharing."
        )) {
          liveStatus.textContent = error.message || "Sharing could not be changed.";
        } else {
          liveStatus.textContent = "The diagram changed elsewhere. Reopen it before changing sharing.";
        }
      } finally {
        dialog.querySelectorAll("button").forEach((button) => { button.disabled = false; });
      }
    };

    const expiryControl = (preserveCurrent) => {
      const label = document.createElement("label");
      label.className = "sharing-expiry";
      const caption = document.createElement("span");
      caption.textContent = "Link lifetime";
      const select = document.createElement("select");
      select.setAttribute("aria-label", "Share link lifetime");
      const choices = preserveCurrent
        ? [["preserve", "Keep current expiry"]]
        : [];
      choices.push(
        ["300", "5 minutes"],
        ["3600", "1 hour"],
        ["86400", "24 hours"],
        ["604800", "7 days"],
        ["2592000", "30 days"],
        ["never", "Never expires"]
      );
      for (const [value, text] of choices) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = text;
        select.appendChild(option);
      }
      select.value = preserveCurrent ? "preserve" : "604800";
      label.append(caption, select);
      return { label, select };
    };

    const selectedExpiry = (select) => select.value === "never"
      ? null
      : Number.parseInt(select.value, 10);

    const renderContent = () => {
      content.replaceChildren();
      const expired = Boolean(state.current.share_expired);
      const status = document.createElement("div");
      status.className = `sharing-status ${state.current.sharing_enabled ? "shared" : expired ? "expired" : "private"}`;
      const statusTitle = document.createElement("strong");
      statusTitle.textContent = state.current.sharing_enabled ? "Shared by link" : expired ? "Share link expired" : "Private";
      const explanation = document.createElement("p");
      explanation.textContent = state.current.sharing_enabled
        ? state.current.share_expires_at
          ? `Anyone with this unguessable link can view and fork the diagram until ${new Date(state.current.share_expires_at).toLocaleString()}.`
          : "Anyone with this unguessable link can view and fork the diagram. It has no automatic expiry."
        : expired
          ? "The old URL is inactive. Enabling sharing creates a new capability instead of reviving it."
          : "Only this guest session and the administrator can access the diagram.";
      status.append(statusTitle, explanation);
      content.appendChild(status);

      const actions = document.createElement("div");
      actions.className = "sharing-actions";
      if (!state.current.sharing_enabled) {
        const expiry = expiryControl(false);
        content.appendChild(expiry.label);
        const enable = mobileBarButton(expired ? "Create new share link" : "Enable share link", "primary");
        enable.onclick = () => changeSharing("enable", selectedExpiry(expiry.select));
        actions.appendChild(enable);
      } else {
        const link = document.createElement("input");
        link.className = "sharing-link";
        link.type = "url";
        link.readOnly = true;
        link.value = state.current.share_url;
        link.setAttribute("aria-label", "Active share link");
        content.appendChild(link);

        const copy = mobileBarButton("Copy link", "primary");
        copy.onclick = () => copyShareLink(state.current.share_url);
        const rotate = mobileBarButton("Rotate link");
        rotate.onclick = () => {
          if (confirm("Rotate this link? The current URL will stop working immediately.")) {
            changeSharing("rotate");
          }
        };
        const disable = mobileBarButton("Disable sharing", "danger");
        disable.onclick = () => {
          if (confirm("Disable sharing? Anyone using the current URL will lose access immediately.")) {
            changeSharing("disable");
          }
        };
        const expiry = expiryControl(true);
        content.appendChild(expiry.label);
        const updateExpiry = mobileBarButton("Update expiry");
        updateExpiry.onclick = () => {
          if (expiry.select.value !== "preserve") {
            changeSharing("set_expiry", selectedExpiry(expiry.select));
          }
        };
        actions.append(copy, rotate, updateExpiry, disable);
      }
      content.appendChild(actions);
    };

    renderContent();
    closeButton.focus();
  }

  async function forkSharedDiagram() {
    const slug = sharedSlugFromPath();
    if (!slug || !state.current || !state.readOnly) return;
    try {
      const created = await api(`/api/diagrams/slug/${slug}/fork`, {
        method: "POST",
        body: "{}"
      });
      history.replaceState(null, "", "/");
      await loadDiagrams();
      await openDiagram(created.id);
      toast("Private fork created in your diagrams.", "success");
    } catch (error) {
      toast(error.message || "The shared diagram could not be forked.", "error");
    }
  }

  function renderEdges() {
    edgesLayer.replaceChildren();
    if (!state.current) return;
    const byKey = new Map(state.current.nodes.map((n) => [n.key, n]));
    for (const edge of state.current.edges) {
      const source = byKey.get(edge.source);
      const target = byKey.get(edge.target);
      if (!source || !target) continue;
      const a = nodeCenter(source);
      const b = nodeCenter(target);
      const selected = state.selected?.type === "edge" && state.selected.key === edge.key;
      const group = svg("g", { class: selected ? "edge-selected" : "", "data-key": edge.key }, edgesLayer);
      const d = `M ${a.x} ${a.y} L ${b.x} ${b.y}`;
      svg("path", { d, class: "edge-hit" }, group);
      svg("path", {
        d,
        class: "edge-path",
        stroke: edge.color || "#94a3b8",
        "marker-end": edge.directed ? "url(#arrow)" : ""
      }, group);
      group.addEventListener("pointerdown", (evt) => {
        evt.stopPropagation();
        canvas.focus({ preventScroll: true });
        if (evt.pointerType === "mouse" && evt.detail === 2 && !state.readOnly) {
          evt.preventDefault();
          state.selected = { type: "edge", key: edge.key };
          render();
          openInlineLabelEditor("edge", edge.key);
          return;
        }
        state.selected = { type: "edge", key: edge.key };
        render();
      });
      group.addEventListener("dblclick", (evt) => {
        if (state.readOnly) return;
        evt.preventDefault();
        evt.stopPropagation();
        state.selected = { type: "edge", key: edge.key };
        render();
        openInlineLabelEditor("edge", edge.key);
      });
      if (edge.label) {
        svg("text", { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 - 10, class: "edge-label" }, group).textContent = shortText(edge.label, 28);
      }
    }
  }

  function renderGeometryLock(group, node) {
    const x = node.x + 9;
    const y = node.y + 8;
    const badge = svg("g", { class: "node-lock-badge", "aria-hidden": "true" }, group);
    svg("path", {
      d: `M ${x + 3} ${y + 8} V ${y + 5} A 4 4 0 0 1 ${x + 11} ${y + 5} V ${y + 8}`,
      class: "node-lock-shackle"
    }, badge);
    svg("rect", { x, y: y + 8, width: 14, height: 10, rx: 2, class: "node-lock-body" }, badge);
  }

  function renderNodeShape(group, node) {
    const selected = isNodeSelected(node.key);
    const locked = nodeGeometryLocked(node);
    group.setAttribute("class", [selected ? "selected" : "", locked ? "geometry-locked" : ""].filter(Boolean).join(" "));
    group.setAttribute("data-geometry-locked", String(locked));
    const accessibleTitle = svg("title", {}, group);
    accessibleTitle.textContent = `${node.title || "Untitled node"}${locked ? ", geometry locked" : ""}`;
    const fill = node.color || colors[node.type] || colors.process;
    if (node.type === "decision") {
      const points = `${node.x + node.width / 2},${node.y} ${node.x + node.width},${node.y + node.height / 2} ${node.x + node.width / 2},${node.y + node.height} ${node.x},${node.y + node.height / 2}`;
      svg("polygon", { points, fill, stroke: "rgba(0,0,0,.32)", "stroke-width": 1.5, class: "node-shape node-outline" }, group);
    } else if (node.type === "database") {
      svg("rect", { x: node.x, y: node.y + 10, width: node.width, height: node.height - 20, rx: 8, fill, stroke: "rgba(0,0,0,.32)", "stroke-width": 1.5, class: "node-shape node-outline" }, group);
      svg("ellipse", { cx: node.x + node.width / 2, cy: node.y + 12, rx: node.width / 2, ry: 13, fill, stroke: "rgba(0,0,0,.32)", "stroke-width": 1.5 }, group);
      svg("path", { d: `M ${node.x} ${node.y + node.height - 12} Q ${node.x + node.width / 2} ${node.y + node.height + 12} ${node.x + node.width} ${node.y + node.height - 12}`, fill: "none", stroke: "rgba(0,0,0,.26)", "stroke-width": 1.5 }, group);
    } else if (node.type === "note") {
      const x = node.x, y = node.y, w = node.width, h = node.height;
      svg("path", { d: `M ${x} ${y} H ${x + w - 20} L ${x + w} ${y + 20} V ${y + h} H ${x} Z`, fill, stroke: "rgba(0,0,0,.32)", "stroke-width": 1.5, class: "node-shape node-outline" }, group);
      svg("path", { d: `M ${x + w - 20} ${y} V ${y + 20} H ${x + w}`, fill: "rgba(255,255,255,.28)" }, group);
    } else {
      const radius = node.type === "process" ? 14 : 7;
      svg("rect", { x: node.x, y: node.y, width: node.width, height: node.height, rx: radius, fill, stroke: "rgba(0,0,0,.32)", "stroke-width": 1.5, class: "node-shape node-outline" }, group);
    }
    svg("text", { x: node.x + node.width / 2, y: node.y + node.height / 2 - 6, class: "node-title" }, group).textContent = shortText(node.title);
    svg("text", { x: node.x + node.width / 2, y: node.y + node.height / 2 + 16, class: "node-subtitle" }, group).textContent = node.type;
    if (locked) renderGeometryLock(group, node);
  }

  function renderNodes() {
    nodesLayer.replaceChildren();
    if (!state.current) return;
    for (const node of state.current.nodes) {
      const group = svg("g", { "data-key": node.key }, nodesLayer);
      renderNodeShape(group, node);
      group.addEventListener("pointerdown", (evt) => {
        evt.stopPropagation();
        canvas.focus({ preventScroll: true });
        if (evt.pointerType === "mouse" && evt.detail === 2 && state.tool === "select" && !state.readOnly) {
          evt.preventDefault();
          cancelLongPress();
          state.dragging = null;
          state.selected = { type: "node", key: node.key };
          render();
          openInlineLabelEditor("node", node.key);
          return;
        }
        state.pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
        if (state.pointers.size === 2) {
          cancelLongPress();
          beginPinch();
          return;
        }
        // A second finger means the user is pinching, not dragging a node.
        if (state.pointers.size > 1) return;
        if (state.tool === "edge") {
          if (!state.edgeSource) {
            state.edgeSource = node.key;
            state.selected = { type: "node", key: node.key };
          } else if (state.edgeSource !== node.key) {
            pushHistory();
            state.current.edges.push({
              key: `edge_${Date.now()}`,
              source: state.edgeSource,
              target: node.key,
              label: "",
              directed: true,
              color: "#94a3b8",
              metadata: {}
            });
            state.edgeSource = null;
          }
          render();
          return;
        }
        // Shift-click on desktop, long-press on touch: both add to the
        // selection rather than replacing it.
        if ((evt.shiftKey || state.multiSelectArmed) && state.tool === "select") {
          toggleNodeSelection(node.key);
          render();
          return;
        }
        if (state.tool === "select") {
          const alreadySelected = selectedNodeKeys().includes(node.key);
          const selectionKeys = alreadySelected ? selectedNodeKeys() : [node.key];
          if (!alreadySelected) state.selected = { type: "node", key: node.key };
          // A locked node remains selectable and editable, but cannot start a
          // geometry gesture. Dragging an unlocked node in a mixed selection
          // moves only the unlocked members.
          if (nodeGeometryLocked(node)) {
            render();
            return;
          }
          pushHistory();
          if (evt.pointerType !== "mouse") {
            armLongPress(() => {
              toggleNodeSelection(node.key);
              state.dragging = null;
              render();
            });
          }
          const p = worldPoint(evt);
          const keys = selectionKeys.filter((key) => {
            const selected = state.current.nodes.find((candidate) => candidate.key === key);
            return selected && !nodeGeometryLocked(selected);
          });
          state.dragging = {
            key: node.key,
            keys,
            dx: p.x - node.x,
            dy: p.y - node.y,
            originals: Object.fromEntries(state.current.nodes.filter((n) => keys.includes(n.key)).map((n) => [n.key, { x: n.x, y: n.y }]))
          };
        } else {
          state.selected = { type: "node", key: node.key };
        }
        render();
      });
      group.addEventListener("dblclick", (evt) => {
        if (state.tool !== "select" || state.readOnly) return;
        evt.preventDefault();
        evt.stopPropagation();
        state.selected = { type: "node", key: node.key };
        render();
        openInlineLabelEditor("node", node.key);
      });
    }
  }

  function renderOverlay() {
    overlayLayer.replaceChildren();
    renderMarquee();
    renderGuides();
    renderResizeHandles();
  }

  function renderMarquee() {
    if (!state.marquee || !state.marquee.moved) return;
    const rect = marqueeRect(state.marquee);
    // The overlay sits inside the zoomed viewport, so a plain stroke width would
    // thicken with the zoom. Divide it back out to keep the band 1px on screen.
    svg("rect", {
      class: "marquee",
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height,
      "stroke-width": 1 / state.zoom,
      "stroke-dasharray": `${5 / state.zoom} ${4 / state.zoom}`
    }, overlayLayer);
  }

  function selectedBatchNodes() {
    if (!state.current || state.selected?.type !== "nodes") return [];
    const keys = new Set(state.selected.keys);
    return state.current.nodes.filter((node) => keys.has(node.key));
  }

  function resetBatchPropertyControls(nodes, signature) {
    el("multi-node-props").dataset.selection = signature;
    el("batch-type").value = "";
    const defaults = [
      ["color", nodes.every((node) => node.color === nodes[0].color) ? nodes[0].color : "#38bdf8"],
      ["width", nodes.every((node) => node.width === nodes[0].width)
        ? Math.round(nodes[0].width)
        : Math.round(nodes.reduce((sum, node) => sum + node.width, 0) / nodes.length)],
      ["height", nodes.every((node) => node.height === nodes[0].height)
        ? Math.round(nodes[0].height)
        : Math.round(nodes.reduce((sum, node) => sum + node.height, 0) / nodes.length)]
    ];
    for (const [property, value] of defaults) {
      el(`batch-${property}-enabled`).checked = false;
      el(`batch-${property}`).value = String(value);
    }
  }

  function updateBatchControlAvailability() {
    for (const property of ["color", "width", "height"]) {
      const enabled = el(`batch-${property}-enabled`);
      enabled.disabled = state.readOnly;
      el(`batch-${property}`).disabled = state.readOnly || !enabled.checked;
    }
    el("batch-type").disabled = state.readOnly;
    el("apply-batch-properties").disabled = state.readOnly;
  }

  function applyBatchProperties() {
    const nodes = selectedBatchNodes();
    if (state.readOnly || nodes.length < 2) return;
    const type = el("batch-type").value;
    const setColor = el("batch-color-enabled").checked;
    const setWidth = el("batch-width-enabled").checked;
    const setHeight = el("batch-height-enabled").checked;
    if (!type && !setColor && !setWidth && !setHeight) {
      toast("Choose at least one batch property.");
      return;
    }
    if (type && !nodeTypes.includes(type)) {
      toast("Choose a supported node type.", "error");
      return;
    }

    const widthInput = el("batch-width");
    const heightInput = el("batch-height");
    if ((setWidth && !widthInput.reportValidity()) || (setHeight && !heightInput.reportValidity())) return;
    const color = el("batch-color").value;
    if (setColor && !/^#[0-9a-f]{6}$/i.test(color)) {
      toast("Choose a valid node colour.", "error");
      return;
    }
    const width = Number(widthInput.value);
    const height = Number(heightInput.value);
    const changed = nodes.some((node) =>
      (type && node.type !== type) ||
      (setColor && node.color !== color) ||
      (setWidth && !nodeGeometryLocked(node) && node.width !== width) ||
      (setHeight && !nodeGeometryLocked(node) && node.height !== height));
    if (!changed) {
      toast("Selected nodes already have those properties.");
      return;
    }

    pushHistory();
    let updated = 0;
    for (const node of nodes) {
      let nodeChanged = false;
      if (type && node.type !== type) { node.type = type; nodeChanged = true; }
      if (setColor && node.color !== color) { node.color = color; nodeChanged = true; }
      if (setWidth && !nodeGeometryLocked(node) && node.width !== width) { node.width = width; nodeChanged = true; }
      if (setHeight && !nodeGeometryLocked(node) && node.height !== height) { node.height = height; nodeChanged = true; }
      if (nodeChanged) updated += 1;
    }
    markDirty();
    el("multi-node-props").dataset.selection = "";
    render();
    toast(`${updated} node${updated === 1 ? "" : "s"} updated.`);
  }

  function renderProperties() {
    const empty = el("selection-empty");
    const nodeProps = el("node-props");
    const multiNodeProps = el("multi-node-props");
    const edgeProps = el("edge-props");
    nodeProps.classList.add("hidden");
    multiNodeProps.classList.add("hidden");
    edgeProps.classList.add("hidden");
    empty.classList.remove("hidden");
    empty.textContent = "No selection";
    if (!state.current || !state.selected) return;
    if (state.selected.type === "node") {
      const node = state.current.nodes.find((n) => n.key === state.selected.key);
      if (!node) return;
      empty.classList.add("hidden");
      nodeProps.classList.remove("hidden");
      el("prop-title").value = node.title;
      el("prop-type").value = node.type;
      el("prop-color").value = node.color;
      el("prop-width").value = Math.round(node.width);
      el("prop-height").value = Math.round(node.height);
      el("prop-width").disabled = state.readOnly || nodeGeometryLocked(node);
      el("prop-height").disabled = state.readOnly || nodeGeometryLocked(node);
    } else if (state.selected.type === "nodes") {
      const nodes = selectedBatchNodes();
      if (nodes.length < 2) return;
      empty.classList.add("hidden");
      multiNodeProps.classList.remove("hidden");
      const minX = Math.min(...nodes.map((node) => node.x));
      const minY = Math.min(...nodes.map((node) => node.y));
      const maxX = Math.max(...nodes.map((node) => node.x + node.width));
      const maxY = Math.max(...nodes.map((node) => node.y + node.height));
      const typeCount = new Set(nodes.map((node) => node.type)).size;
      const colorCount = new Set(nodes.map((node) => node.color)).size;
      const lockedCount = nodes.filter(nodeGeometryLocked).length;
      el("multi-node-count").textContent = `${nodes.length} nodes selected`;
      el("multi-node-summary").textContent = `${typeCount} type${typeCount === 1 ? "" : "s"} · ${colorCount} color${colorCount === 1 ? "" : "s"} · ${lockedCount} locked · ${Math.round(maxX - minX)} × ${Math.round(maxY - minY)} bounds`;
      const signature = `${state.current.id || "new"}:${nodes.map((node) => node.key).sort().join("|")}`;
      if (multiNodeProps.dataset.selection !== signature) {
        resetBatchPropertyControls(nodes, signature);
      }
      updateBatchControlAvailability();
    } else if (state.selected.type === "edge") {
      const edge = state.current.edges.find((e) => e.key === state.selected.key);
      if (!edge) return;
      empty.classList.add("hidden");
      edgeProps.classList.remove("hidden");
      el("edge-label").value = edge.label || "";
      el("edge-color").value = edge.color || "#94a3b8";
      el("edge-directed").checked = !!edge.directed;
    }
  }

  function render() {
    viewport.setAttribute("transform", `translate(${state.pan.x} ${state.pan.y}) scale(${state.zoom})`);
    el("zoom-label").textContent = `${Math.round(state.zoom * 100)}%`;
    if (state.current) {
      el("diagram-title").value = state.readOnly ? `${state.current.title} (read-only)` : state.current.title;
    }
    const shareButton = el("share-diagram");
    if (shareButton) {
      shareButton.disabled = !state.current;
      shareButton.classList.toggle("active", Boolean(state.current?.sharing_enabled));
      shareButton.title = !state.current ? "Open a diagram to manage sharing"
        : state.readOnly ? "Copy share link"
          : state.current.sharing_enabled ? "Manage active share link"
            : state.current.share_expired ? "Share link expired" : "Enable sharing";
    }
    const forkButton = el("fork-shared");
    if (forkButton) forkButton.hidden = !(state.current && state.readOnly && sharedSlugFromPath());
    renderEdges();
    renderNodes();
    renderOverlay();
    renderProperties();
    renderMinimap();
    positionInlineLabelEditor();
    refreshMobileActionBar();
    updateSelectionAnnouncement();
  }

  function renderMinimap() {
    const minimap = el("minimap");
    if (!minimap) return;
    minimap.replaceChildren();
    if (!state.current || !state.current.nodes.length) return;
    const xs = state.current.nodes.flatMap((n) => [n.x, n.x + n.width]);
    const ys = state.current.nodes.flatMap((n) => [n.y, n.y + n.height]);
    const minX = Math.min(...xs) - 80;
    const minY = Math.min(...ys) - 80;
    const maxX = Math.max(...xs) + 80;
    const maxY = Math.max(...ys) + 80;
    const w = Math.max(1, maxX - minX);
    const h = Math.max(1, maxY - minY);
    const mini = svg("svg", { viewBox: `${minX} ${minY} ${w} ${h}` }, minimap);
    const byKey = new Map(state.current.nodes.map((n) => [n.key, n]));
    for (const edge of state.current.edges) {
      const a = byKey.get(edge.source);
      const b = byKey.get(edge.target);
      if (!a || !b) continue;
      const ca = nodeCenter(a);
      const cb = nodeCenter(b);
      svg("line", { x1: ca.x, y1: ca.y, x2: cb.x, y2: cb.y, stroke: "#64748b", "stroke-width": 6, "stroke-linecap": "round" }, mini);
    }
    for (const node of state.current.nodes) {
      svg("rect", { x: node.x, y: node.y, width: node.width, height: node.height, rx: 12, fill: node.color || "#38bdf8", stroke: isNodeSelected(node.key) ? "#ffffff" : "rgba(0,0,0,.35)", "stroke-width": isNodeSelected(node.key) ? 8 : 3 }, mini);
    }
  }

  function visibleDiagrams() {
    const needle = state.filter.trim().toLowerCase();
    if (!needle) return state.diagrams;
    return state.diagrams.filter((d) =>
      (d.title || "").toLowerCase().includes(needle) ||
      (d.description || "").toLowerCase().includes(needle));
  }

  function renderDiagramList() {
    const list = el("diagram-list");
    list.replaceChildren();
    const items = visibleDiagrams();
    if (!items.length) {
      const empty = document.createElement("div");
      empty.className = "muted";
      empty.textContent = !state.diagrams.length ? "No diagrams yet."
        : state.diagramsHasMore ? "No loaded diagrams match that search."
        : "No diagrams match that search.";
      list.appendChild(empty);
    }
    for (const d of items) {
      const item = document.createElement("div");
      item.className = `diagram-item ${state.current?.id === d.id ? "active" : ""}`;
      const row = document.createElement("div");
      row.className = "diagram-row";
      const name = document.createElement("div");
      name.className = "diagram-name";
      name.textContent = d.title;
      const actions = document.createElement("div");
      actions.className = "small-actions";
      const open = mobileBarButton("Open");
      open.dataset.act = "open";
      open.onclick = () => openDiagram(d.id);
      const duplicate = mobileBarButton("Copy");
      duplicate.dataset.act = "dup";
      duplicate.onclick = () => duplicateDiagram(d.id);
      const remove = mobileBarButton("Del", "danger");
      remove.dataset.act = "del";
      remove.onclick = () => deleteDiagram(d);
      actions.append(open, duplicate, remove);
      row.append(name, actions);
      const counts = document.createElement("div");
      counts.className = "meta";
      counts.textContent = `${d.node_count || 0} nodes, ${d.edge_count || 0} edges`;
      const updated = document.createElement("div");
      updated.className = "meta";
      updated.textContent = `Updated ${d.updated_at || ""}`;
      item.append(row, counts, updated);
      list.appendChild(item);
    }

    // Shown while filtering too: the filter only sees diagrams already loaded,
    // so hiding this would leave a search looking conclusive when it is not.
    if (state.diagramsHasMore) {
      const more = document.createElement("button");
      more.type = "button";
      more.className = "load-more";
      more.textContent = `Load more (${state.diagrams.length} of ${state.diagramsTotal})`;
      more.onclick = async () => {
        more.disabled = true;
        more.textContent = "Loading...";
        try {
          await loadMoreDiagrams();
        } catch (err) {
          more.disabled = false;
          more.textContent = "Load more";
          toast(err.message || "Could not load more diagrams");
        }
      };
      list.appendChild(more);
    }
  }

  function renderTemplateList() {
    const list = el("template-list");
    if (!list) return;
    list.replaceChildren();
    if (!state.templates.length) {
      const empty = document.createElement("div");
      empty.className = "muted";
      empty.textContent = "Templates unavailable.";
      list.appendChild(empty);
      return;
    }
    for (const template of state.templates) {
      const item = document.createElement("div");
      item.className = "diagram-item";
      const row = document.createElement("div");
      row.className = "diagram-row";
      const name = document.createElement("div");
      name.className = "diagram-name";
      name.textContent = template.title;
      const actions = document.createElement("div");
      actions.className = "small-actions";
      const use = mobileBarButton("Use");
      use.dataset.act = "use";
      use.onclick = () => createFromTemplate(template.key);
      actions.appendChild(use);
      row.append(name, actions);
      const summary = document.createElement("div");
      summary.className = "meta";
      summary.textContent = `${template.category || "Template"} - ${template.node_count || 0} nodes, ${template.edge_count || 0} edges`;
      const description = document.createElement("div");
      description.className = "meta";
      description.textContent = template.description || "";
      item.append(row, summary, description);
      list.appendChild(item);
    }
  }

  // The list endpoint is paged. A guest never fills a page, so this is a single
  // request for them; the admin view loads more on demand rather than pulling
  // the whole table into the sidebar.
  async function loadDiagrams() {
    const data = await api("/api/diagrams");
    state.diagrams = data.diagrams || [];
    state.diagramsTotal = typeof data.total === "number" ? data.total : state.diagrams.length;
    state.diagramsHasMore = Boolean(data.has_more);
    renderDiagramList();
    if (!state.current && !sharedSlugFromPath() && state.diagrams[0]) {
      await openDiagram(state.diagrams[0].id);
    }
  }

  async function loadMoreDiagrams() {
    if (!state.diagramsHasMore) return;
    const data = await api(`/api/diagrams?offset=${state.diagrams.length}`);
    const page = data.diagrams || [];
    // A diagram saved between the two fetches moves to the top of the ordering
    // and would arrive again in this page, so drop ids already held.
    const seen = new Set(state.diagrams.map((d) => d.id));
    state.diagrams = state.diagrams.concat(page.filter((d) => !seen.has(d.id)));
    state.diagramsTotal = typeof data.total === "number" ? data.total : state.diagrams.length;
    state.diagramsHasMore = Boolean(data.has_more) && page.length > 0;
    renderDiagramList();
  }

  async function loadTemplates() {
    const data = await api("/api/templates");
    state.templates = data.templates || [];
    renderTemplateList();
  }

  async function openDiagram(id) {
    endKeyboardNudge();
    state.current = await api(`/api/diagrams/${id}`);
    setReadOnly(state.current.can_edit === false);
    maybeRestoreDraft();
    state.history = [];
    state.future = [];
    state.selected = null;
    state.edgeSource = null;
    if (state.current.nodes?.length) zoomToFit();
    else render();
    await loadVersions();
    renderDiagramList();
  }

  async function openDiagramBySlug(slug) {
    endKeyboardNudge();
    state.current = await api(`/api/diagrams/slug/${slug}`);
    setReadOnly(state.current.can_edit === false);
    maybeRestoreDraft();
    state.history = [];
    state.future = [];
    state.selected = null;
    state.edgeSource = null;
    if (state.current.nodes?.length) zoomToFit();
    else render();
    await loadVersions();
    renderDiagramList();
  }

  async function newDiagram() {
    const created = await api("/api/diagrams", { method: "POST", body: "" });
    await loadDiagrams();
    await openDiagram(created.id);
  }

  async function loadSample() {
    await createFromTemplate("cloud-architecture");
  }

  async function createFromTemplate(key) {
    const created = await api(`/api/templates/${key}/create`, { method: "POST", body: "{}" });
    await loadDiagrams();
    await openDiagram(created.id);
  }

  async function saveDiagram() {
    if (!state.current || state.readOnly) return;
    endKeyboardNudge();
    const draftId = state.current.id;
    state.current.title = el("diagram-title").value.trim() || "Untitled diagram";
    try {
      state.current = await api(`/api/diagrams/${state.current.id}`, {
        method: "PUT",
        headers: { "If-Match": diagramEtag(state.current) },
        body: JSON.stringify(state.current)
      });
      clearDraft(draftId);
      state.history = [];
      state.future = [];
      await loadDiagrams();
      await loadVersions();
      render();
    } catch (error) {
      if (!handleRevisionConflict(error)) {
        toast(error.message || "Could not save the diagram.", "error");
      }
    }
  }

  async function duplicateDiagram(id) {
    const created = await api(`/api/diagrams/${id}/duplicate`, { method: "POST", body: "{}" });
    await loadDiagrams();
    await openDiagram(created.id);
  }

  async function deleteDiagram(diagram) {
    const id = diagram?.id;
    if (!id || (state.readOnly && state.current?.id === id)) return;
    if (!confirm("Delete this diagram?")) return;
    try {
      await api(`/api/diagrams/${id}`, {
        method: "DELETE",
        headers: { "If-Match": diagramEtag(diagram) }
      });
      if (state.current?.id === id) state.current = null;
      await loadDiagrams();
      render();
    } catch (error) {
      if (!handleRevisionConflict(error, state.current?.id === id)) {
        toast(error.message || "Could not delete the diagram.", "error");
      }
    }
  }

  async function loadVersions() {
    const list = el("version-list");
    list.replaceChildren();
    if (!state.current) return;
    if (state.readOnly) {
      const empty = document.createElement("div");
      empty.className = "muted";
      empty.textContent = "Version history is only available to the owner.";
      list.appendChild(empty);
      return;
    }
    const data = await api(`/api/diagrams/${state.current.id}/versions`);
    for (const v of data.versions || []) {
      const item = document.createElement("div");
      item.className = "version-item";
      const version = document.createElement("strong");
      version.textContent = `v${v.version_number}`;
      const created = document.createElement("span");
      created.className = "meta";
      created.textContent = v.created_at;
      const note = document.createElement("span");
      note.className = "meta";
      note.textContent = v.note || "";
      const actions = document.createElement("div");
      actions.className = "small-actions";
      const diff = mobileBarButton("Diff");
      diff.dataset.act = "diff";
      const restore = mobileBarButton("Restore");
      restore.dataset.act = "restore";
      actions.append(diff, restore);
      item.append(version, created, note, actions);
      diff.onclick = async () => {
        const snapshot = await api(`/api/diagrams/${state.current.id}/versions/${v.id}`);
        showDiff(v, diffDiagrams(snapshot, state.current));
      };
      restore.onclick = async () => {
        if (!confirm(`Restore version ${v.version_number}?`)) return;
        if (state.readOnly) return;
        const diagramId = state.current.id;
        try {
          state.current = await api(`/api/diagrams/${diagramId}/restore/${v.id}`, {
            method: "POST",
            headers: { "If-Match": diagramEtag(state.current) },
            body: "{}"
          });
          clearDraft(diagramId);
          state.history = [];
          state.future = [];
          await loadDiagrams();
          await loadVersions();
          render();
        } catch (error) {
          if (!handleRevisionConflict(error)) {
            toast(error.message || "Could not restore the version.", "error");
          }
        }
      };
      list.appendChild(item);
    }
  }

  function createNodeAt(point) {
    if (!state.current || state.readOnly) return;
    pushHistory();
    const type = el("node-type").value;
    const size = type === "decision" ? { width: 150, height: 96 } : { width: 170, height: 82 };
    state.current.nodes.push({
      key: `node_${Date.now()}`,
      type,
      title: type.charAt(0).toUpperCase() + type.slice(1),
      x: snapValue(point.x),
      y: snapValue(point.y),
      width: size.width,
      height: size.height,
      color: colors[type] || colors.process,
      metadata: {}
    });
    state.selected = { type: "node", key: state.current.nodes[state.current.nodes.length - 1].key };
    render();
  }

  function deleteSelected() {
    if (!state.current || !state.selected || state.readOnly) return;
    pushHistory();
    if (state.selected.type === "node" || state.selected.type === "nodes") {
      removeNodes(selectedNodeKeys());
    } else {
      state.current.edges = state.current.edges.filter((e) => e.key !== state.selected.key);
    }
    state.selected = null;
    render();
  }

  function setTool(tool) {
    state.tool = tool;
    state.edgeSource = null;
    document.querySelectorAll(".toolbox button[data-tool]").forEach((btn) => btn.classList.toggle("active", btn.dataset.tool === tool));
    refreshMobileActionBar();
  }

  function clipboardFromSelection() {
    if (!state.current || !state.selected) return null;
    const keys = selectedNodeKeys();
    if (!keys.length) return null;
    const keySet = new Set(keys);
    return {
      nodes: state.current.nodes.filter((node) => keySet.has(node.key))
        .map((node) => ({ ...node, metadata: { ...(node.metadata || {}) } })),
      edges: state.current.edges.filter((edge) => keySet.has(edge.source) && keySet.has(edge.target))
        .map((edge) => ({ ...edge, metadata: { ...(edge.metadata || {}) } }))
    };
  }

  function copySelected() {
    const clipboard = clipboardFromSelection();
    if (clipboard) state.clipboard = clipboard;
  }

  function removeNodes(keys) {
    const removed = new Set(keys);
    state.current.nodes = state.current.nodes.filter((node) => !removed.has(node.key));
    state.current.edges = state.current.edges.filter((edge) =>
      !removed.has(edge.source) && !removed.has(edge.target));
  }

  function cutSelected() {
    if (!state.current || state.readOnly) return;
    const clipboard = clipboardFromSelection();
    if (!clipboard?.nodes?.length) return;
    state.clipboard = clipboard;
    pushHistory();
    removeNodes(clipboard.nodes.map((node) => node.key));
    state.selected = null;
    state.edgeSource = null;
    render();
    toast(`${clipboard.nodes.length} node${clipboard.nodes.length === 1 ? "" : "s"} cut.`);
  }

  function nextAvailableKey(base, used, nonce) {
    const safeBase = String(base || "object").replace(/[^A-Za-z0-9_-]/g, "_") || "object";
    for (let attempt = 0; ; attempt += 1) {
      const suffix = `_${nonce}_${attempt.toString(36)}`;
      const key = `${safeBase.slice(0, 64 - suffix.length)}${suffix}`;
      if (used.has(key)) continue;
      used.add(key);
      return key;
    }
  }

  function pasteClipboard(clipboard) {
    if (!state.current || state.readOnly || !clipboard?.nodes?.length) return false;
    pushHistory();
    const nonce = Date.now().toString(36);
    const nodeKeys = new Set(state.current.nodes.map((node) => node.key));
    const edgeKeys = new Set(state.current.edges.map((edge) => edge.key));
    const keyMap = new Map();
    const pastedKeys = [];
    for (const node of clipboard.nodes) {
      const key = nextAvailableKey(node.key, nodeKeys, nonce);
      keyMap.set(node.key, key);
      pastedKeys.push(key);
      state.current.nodes.push({
        ...node,
        key,
        title: `${String(node.title || "Untitled node").slice(0, 155)} Copy`,
        x: clamp(node.x + 48, -100000, 100000),
        y: clamp(node.y + 48, -100000, 100000),
        metadata: { ...(node.metadata || {}) }
      });
    }
    for (const edge of clipboard.edges || []) {
      if (!keyMap.has(edge.source) || !keyMap.has(edge.target)) continue;
      state.current.edges.push({
        ...edge,
        key: nextAvailableKey(edge.key, edgeKeys, nonce),
        source: keyMap.get(edge.source),
        target: keyMap.get(edge.target),
        metadata: { ...(edge.metadata || {}) }
      });
    }
    selectNodeKeys(pastedKeys);
    render();
    return true;
  }

  function pasteSelected() {
    pasteClipboard(state.clipboard);
  }

  function duplicateSelected() {
    const clipboard = clipboardFromSelection();
    if (pasteClipboard(clipboard)) {
      toast(`${clipboard.nodes.length} node${clipboard.nodes.length === 1 ? "" : "s"} duplicated.`);
    }
  }

  function autoLayout() {
    if (!state.current || state.readOnly) return;
    const nodes = state.current.nodes;
    if (!nodes.some((node) => !nodeGeometryLocked(node))) {
      toast("Every node is geometry locked.");
      return;
    }
    pushHistory();
    const incoming = new Map(nodes.map((n) => [n.key, 0]));
    const outgoing = new Map(nodes.map((n) => [n.key, []]));
    for (const edge of state.current.edges) {
      if (incoming.has(edge.target) && outgoing.has(edge.source)) {
        incoming.set(edge.target, incoming.get(edge.target) + 1);
        outgoing.get(edge.source).push(edge.target);
      }
    }
    const level = new Map();
    const queue = nodes.filter((n) => incoming.get(n.key) === 0).map((n) => n.key);
    if (!queue.length && nodes[0]) queue.push(nodes[0].key);
    for (const key of queue) level.set(key, 0);
    while (queue.length) {
      const key = queue.shift();
      for (const next of outgoing.get(key) || []) {
        const nextLevel = Math.max(level.get(next) ?? 0, (level.get(key) ?? 0) + 1);
        if (!level.has(next) || nextLevel > level.get(next)) {
          level.set(next, nextLevel);
          queue.push(next);
        }
      }
    }
    for (const node of nodes) {
      if (!level.has(node.key)) level.set(node.key, 0);
    }
    const groups = new Map();
    for (const node of nodes) {
      const l = level.get(node.key);
      if (!groups.has(l)) groups.set(l, []);
      groups.get(l).push(node);
    }
    [...groups.keys()].sort((a, b) => a - b).forEach((l) => {
      groups.get(l).forEach((node, i) => {
        if (nodeGeometryLocked(node)) return;
        node.x = 80 + l * 260;
        node.y = 80 + i * 140;
      });
    });
    render();
  }

  function bindProperty(id, apply, allowed = () => true) {
    el(id).addEventListener("change", () => {
      if (!state.current || !state.selected || state.readOnly || !allowed()) return;
      pushHistory();
      apply();
      render();
    });
  }

  function initBindings() {
    nodeTypes.forEach((type) => {
      for (const selectId of ["prop-type", "batch-type"]) {
        const opt = document.createElement("option");
        opt.value = type;
        opt.textContent = type;
        el(selectId).appendChild(opt);
      }
    });
    for (const property of ["color", "width", "height"]) {
      el(`batch-${property}-enabled`).addEventListener("change", updateBatchControlAvailability);
    }
    el("apply-batch-properties").onclick = applyBatchProperties;
    document.querySelectorAll(".toolbox button[data-tool]").forEach((btn) => btn.onclick = () => {
      setTool(btn.dataset.tool);
      if (btn.dataset.tool === "node" && state.current) {
        createNodeAt(viewportCenterPoint());
      }
    });
    el("new-diagram").onclick = newDiagram;
    el("load-sample").onclick = loadSample;
    el("share-diagram").onclick = shareDiagram;
    const topActions = document.querySelector(".top-actions");
    if (topActions && !el("fork-shared")) {
      const fork = document.createElement("button");
      fork.type = "button";
      fork.id = "fork-shared";
      fork.textContent = "Fork";
      fork.title = "Create a private copy";
      fork.hidden = true;
      fork.onclick = forkSharedDiagram;
      el("share-diagram")?.after(fork);
    }
    el("save-diagram").onclick = saveDiagram;
    el("refresh-diagrams").onclick = loadDiagrams;
    el("delete-selected").onclick = deleteSelected;
    el("undo").onclick = undo;
    el("redo").onclick = redo;
    el("copy-selected").onclick = copySelected;
    el("paste-selected").onclick = pasteSelected;
    el("auto-layout").onclick = autoLayout;
    el("snap-toggle").onclick = () => {
      state.snap = !state.snap;
      el("snap-toggle").classList.toggle("active", state.snap);
    };
    el("grid-size").onchange = () => applyGridSize(el("grid-size").value);
    el("zoom-in").onclick = () => { state.zoom = Math.min(2.5, state.zoom * 1.15); render(); };
    el("zoom-out").onclick = () => { state.zoom = Math.max(0.25, state.zoom / 1.15); render(); };
    el("export-json").onclick = () => { if (state.current) window.open(`/api/diagrams/${state.current.id}/export.json`, "_blank"); };
    el("export-svg").onclick = exportSvg;

    // Added here rather than in the server markup so the shell stays stable.
    const actions = document.querySelector(".top-actions");
    if (actions) {
      const commands = document.createElement("button");
      commands.type = "button";
      commands.id = "show-command-palette";
      commands.textContent = "Commands";
      commands.title = "Open command palette (Ctrl/Cmd+K)";
      commands.onclick = () => openCommandPalette();
      actions.prepend(commands);

      const mmd = document.createElement("button");
      mmd.type = "button";
      mmd.id = "export-mermaid";
      mmd.textContent = "Mermaid";
      mmd.title = "Export as a Mermaid flowchart";
      mmd.onclick = exportMermaid;
      actions.insertBefore(mmd, el("export-png"));

      const help = document.createElement("button");
      help.type = "button";
      help.id = "show-shortcuts";
      help.textContent = "?";
      help.title = "Keyboard shortcuts";
      help.setAttribute("aria-label", "Keyboard shortcuts");
      help.onclick = toggleShortcuts;
      actions.appendChild(help);
    }
    el("export-png").onclick = exportPng;
    el("make-version").onclick = async () => {
      if (!state.current) return;
      if (state.readOnly) return;
      await api(`/api/diagrams/${state.current.id}/versions`, { method: "POST", body: JSON.stringify({ note: "manual snapshot" }) });
      await loadVersions();
    };
    el("import-json").onclick = () => el("import-file").click();
    el("import-file").onchange = async (evt) => {
      const file = evt.target.files[0];
      if (!file) return;
      if (file.size > 1048576) {
        toast("Import file is too large.", "error");
        evt.target.value = "";
        return;
      }
      const text = await file.text();
      const created = await api("/api/diagrams/import", { method: "POST", body: text });
      await loadDiagrams();
      await openDiagram(created.id);
      evt.target.value = "";
    };

    el("diagram-title").addEventListener("change", () => {
      if (!state.current || state.readOnly) return;
      pushHistory();
      state.current.title = el("diagram-title").value.trim() || "Untitled diagram";
      markDirty();
      renderDiagramList();
    });

    bindProperty("prop-title", () => state.current.nodes.find((n) => n.key === state.selected.key).title = el("prop-title").value);
    bindProperty("prop-type", () => {
      const node = state.current.nodes.find((n) => n.key === state.selected.key);
      node.type = el("prop-type").value;
      node.color = colors[node.type] || node.color;
    });
    bindProperty("prop-color", () => state.current.nodes.find((n) => n.key === state.selected.key).color = el("prop-color").value);
    const selectedGeometryEditable = () => {
      const node = state.current?.nodes.find((candidate) => candidate.key === state.selected?.key);
      return Boolean(node) && !nodeGeometryLocked(node);
    };
    bindProperty("prop-width", () => state.current.nodes.find((n) => n.key === state.selected.key).width = Number(el("prop-width").value), selectedGeometryEditable);
    bindProperty("prop-height", () => state.current.nodes.find((n) => n.key === state.selected.key).height = Number(el("prop-height").value), selectedGeometryEditable);
    bindProperty("edge-label", () => state.current.edges.find((e) => e.key === state.selected.key).label = el("edge-label").value);
    bindProperty("edge-color", () => state.current.edges.find((e) => e.key === state.selected.key).color = el("edge-color").value);
    bindProperty("edge-directed", () => state.current.edges.find((e) => e.key === state.selected.key).directed = el("edge-directed").checked);

    // Pointer events replace the mouse-only handlers so touch and pen work the
    // same way. canvas CSS sets touch-action:none, so the browser hands us the
    // raw gestures instead of scrolling or zooming the page itself.
    canvas.addEventListener("pointerdown", (evt) => {
      state.pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
      if (state.pointers.size === 2) {
        cancelLongPress();
        state.marquee = null;
        beginPinch();
        return;
      }
      if (state.pointers.size > 2) return;

      // Middle mouse, the pan tool, or a touch drag on empty canvas all pan.
      // On touch the select tool pans rather than rubber-bands: a finger has no
      // Shift key to extend with and panning is what a drag means everywhere
      // else on a phone.
      const panGesture = evt.button === 1 || state.tool === "pan" ||
        (evt.pointerType !== "mouse" && state.tool === "select");
      if (panGesture) {
        state.panning = { x: evt.clientX - state.pan.x, y: evt.clientY - state.pan.y, moved: false };
        canvas.setPointerCapture(evt.pointerId);
        return;
      }
      if (state.tool === "node") {
        createNodeAt(worldPoint(evt));
        return;
      }
      if (state.tool === "select") {
        const p = worldPoint(evt);
        // The selection is deliberately left alone here. A drag that ends up
        // empty and a plain click both clear it on release, and clearing now
        // would throw away the selection a Shift-drag is meant to extend.
        state.marquee = {
          x0: p.x, y0: p.y, x1: p.x, y1: p.y,
          clientX: evt.clientX, clientY: evt.clientY,
          additive: evt.shiftKey,
          baseKeys: selectedNodeKeys(),
          moved: false
        };
        canvas.setPointerCapture(evt.pointerId);
        return;
      }
      state.selected = null;
      state.edgeSource = null;
      render();
    });

    canvas.addEventListener("pointermove", (evt) => {
      if (!state.pointers.has(evt.pointerId)) return;
      state.pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
      if (state.pinch) updatePinch();
    });

    // Node drags start on the node group and can travel outside it, so the move
    // and release handlers stay on the window.
    window.addEventListener("pointermove", (evt) => {
      if (state.pointers.has(evt.pointerId)) {
        state.pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
      }
      if (state.pinch) return;

      if (state.resizing && state.current) {
        if (state.readOnly) return;
        cancelLongPress();
        const node = state.current.nodes.find((n) => n.key === state.resizing.key);
        if (node && !nodeGeometryLocked(node)) {
          applyResize(node, state.resizing, worldPoint(evt));
          render();
        }
      } else if (state.dragging && state.current) {
        if (state.readOnly) return;
        cancelLongPress();
        const node = state.current.nodes.find((n) => n.key === state.dragging.key);
        if (node) {
          const p = worldPoint(evt);
          const nextX = snapValue(p.x - state.dragging.dx);
          const nextY = snapValue(p.y - state.dragging.dy);
          let deltaX = nextX - state.dragging.originals[state.dragging.key].x;
          let deltaY = nextY - state.dragging.originals[state.dragging.key].y;
          // Alignment is applied after the grid, and wins: a node the user is
          // visibly lining up with another should land on it, not one grid step
          // short of it.
          const keys = state.dragging.keys || [state.dragging.key];
          const align = alignmentAdjust(movingBox(state.dragging, deltaX, deltaY), keys);
          deltaX += align.dx;
          deltaY += align.dy;
          state.guides = guideSpans(align.matches, movingBox(state.dragging, deltaX, deltaY));
          for (const key of keys) {
            const moving = state.current.nodes.find((n) => n.key === key);
            const original = state.dragging.originals[key];
            if (moving && original && !nodeGeometryLocked(moving)) {
              moving.x = original.x + deltaX;
              moving.y = original.y + deltaY;
            }
          }
          render();
        }
      } else if (state.panning) {
        state.panning.moved = true;
        state.pan.x = evt.clientX - state.panning.x;
        state.pan.y = evt.clientY - state.panning.y;
        render();
      } else if (state.marquee) {
        const m = state.marquee;
        // A couple of pixels of travel while clicking is normal. Below the
        // threshold this is still a click, so no band is drawn and the
        // selection is left for the release handler to clear.
        if (!m.moved && Math.hypot(evt.clientX - m.clientX, evt.clientY - m.clientY) > MARQUEE_MIN_DRAG) {
          m.moved = true;
        }
        const p = worldPoint(evt);
        m.x1 = p.x;
        m.y1 = p.y;
        if (m.moved) applyMarqueeSelection(m);
        render();
      }
    });

    const endPointer = (evt) => {
      state.pointers.delete(evt.pointerId);
      cancelLongPress();
      if (state.pointers.size < 2) state.pinch = null;
      if (state.pointers.size === 0) state.multiSelectArmed = false;
      if ((state.dragging || state.resizing) && state.current && !state.readOnly) markDirty();
      const hadGuides = state.guides.length > 0;
      state.dragging = null;
      state.resizing = null;
      state.panning = null;
      state.guides = [];
      if (hadGuides) render();
      if (state.marquee) {
        // A click that never became a drag still means "deselect", which is what
        // clicking empty canvas did before the rubber-band existed. Shift-click
        // is excluded: it is an attempt to extend, not to clear.
        if (!state.marquee.moved && !state.marquee.additive) {
          state.selected = null;
          state.edgeSource = null;
        }
        state.marquee = null;
        render();
      }
    };
    window.addEventListener("pointerup", endPointer);
    window.addEventListener("pointercancel", endPointer);

    canvas.addEventListener("wheel", (evt) => {
      evt.preventDefault();
      zoomAround(evt.clientX, evt.clientY, state.zoom * (evt.deltaY < 0 ? 1.08 : 0.92));
      render();
    }, { passive: false });

    window.addEventListener("keydown", (evt) => {
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "k") {
        evt.preventDefault();
        if (document.querySelector(".command-palette-overlay")) closeDialog(".command-palette-overlay");
        else openCommandPalette();
        return;
      }
      if (evt.altKey && !evt.ctrlKey && !evt.metaKey && evt.key.toLowerCase() === "o") {
        evt.preventDefault();
        if (!document.querySelector('[role="dialog"]')) openDiagramOutline();
        return;
      }
      if (evt.target.matches("input, textarea, select") || evt.target.isContentEditable) return;
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "a" && !evt.altKey &&
          !document.querySelector('[role="dialog"]')) {
        evt.preventDefault();
        selectAllNodes();
        return;
      }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "d" && !evt.altKey &&
          !document.querySelector('[role="dialog"]')) {
        evt.preventDefault();
        duplicateSelected();
        return;
      }
      if (evt.shiftKey && evt.code === "Digit2" && !evt.ctrlKey && !evt.metaKey && !evt.altKey &&
          !document.querySelector('[role="dialog"]') && selectedFrameNodes().length) {
        evt.preventDefault();
        frameSelection();
        return;
      }
      if (["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(evt.key) &&
          !evt.ctrlKey && !evt.metaKey && !evt.altKey &&
          (evt.target === canvas || evt.target === document.body) &&
          !document.querySelector('[role="dialog"]') && selectedNodeKeys().length && !state.readOnly) {
        evt.preventDefault();
        const step = (state.snap ? state.gridSize : 1) * (evt.shiftKey ? 5 : 1);
        nudgeSelectedNodes(
          evt.key === "ArrowLeft" ? -step : evt.key === "ArrowRight" ? step : 0,
          evt.key === "ArrowUp" ? -step : evt.key === "ArrowDown" ? step : 0
        );
        return;
      }
      const editWithEnter = evt.key === "Enter" && (evt.target === canvas || evt.target === document.body);
      if ((evt.key === "F2" || editWithEnter) && !evt.ctrlKey && !evt.metaKey && !evt.altKey &&
          !document.querySelector('[role="dialog"]') && editableSelectedLabelTarget() && !state.readOnly) {
        evt.preventDefault();
        openInlineLabelEditor();
        return;
      }
      if (evt.key === "/" && !evt.ctrlKey && !evt.metaKey && !evt.altKey) {
        evt.preventDefault();
        openNodeNavigator();
        return;
      }
      if (evt.key === "Delete" || evt.key === "Backspace") deleteSelected();
      if (evt.key === "Escape") { state.selected = null; state.edgeSource = null; render(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "s") { evt.preventDefault(); saveDiagram(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "c") { evt.preventDefault(); copySelected(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "x" && !evt.altKey &&
          !document.querySelector('[role="dialog"]')) { evt.preventDefault(); cutSelected(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "v") { evt.preventDefault(); pasteSelected(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "z") { evt.preventDefault(); evt.shiftKey ? redo() : undo(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "y") { evt.preventDefault(); redo(); }
      if (evt.key === "?") { evt.preventDefault(); toggleShortcuts(); }
      if (evt.key === "Escape") closeDialog(".shortcuts-overlay");
    });

    window.addEventListener("beforeunload", (evt) => {
      if (!state.dirty) return;
      evt.preventDefault();
      evt.returnValue = "";
    });
  }

  initBindings();
  initSheets();
  initMobileActionBar();
  initTheme();
  initGridSize();
  initSearch();
  Promise.all([loadTemplates(), loadDiagrams()])
    .then(() => {
      const slug = sharedSlugFromPath();
      if (slug) return openDiagramBySlug(slug);
    })
    .catch((err) => toast(err.message, "error"));
  render();
})();
