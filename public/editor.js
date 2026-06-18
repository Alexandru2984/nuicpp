(() => {
  if (!document.querySelector('link[href="/styles.css"]')) {
    const css = document.createElement("link");
    css.rel = "stylesheet";
    css.href = "/styles.css";
    document.head.appendChild(css);
  }

  const svgNS = "http://www.w3.org/2000/svg";
  const nodeTypes = ["process", "decision", "database", "service", "api", "note", "external"];
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
    templates: [],
    current: null,
    selected: null,
    readOnly: false,
    tool: "select",
    snap: true,
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
    draftTimer: null
  };

  const el = (id) => document.getElementById(id);
  const canvas = el("canvas");
  const viewport = el("viewport");
  const nodesLayer = el("nodes");
  const edgesLayer = el("edges");

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
    const headers = { "Content-Type": "application/json", ...(options.headers || {}) };
    if (!["GET", "HEAD", "OPTIONS"].includes(method)) {
      headers["X-CSRF-Token"] = await ensureCsrfToken();
    }
    return fetch(path, {
      credentials: "same-origin",
      headers,
      ...options
    }).then(async (res) => {
      if (res.status === 401) {
        location.href = "/login";
        return Promise.reject(new Error("authentication required"));
      }
      const text = await res.text();
      const data = text ? JSON.parse(text) : {};
      if (!res.ok) throw new Error(data.error || res.statusText);
      return data;
    });
  }

  function cloneDiagram() {
    return JSON.parse(JSON.stringify(state.current));
  }

  function pushHistory() {
    if (!state.current || state.readOnly) return;
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
          can_edit: state.current.can_edit,
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
    if (!state.history.length || !state.current) return;
    state.future.push(cloneDiagram());
    restoreSnapshot(state.history.pop());
    markDirty();
  }

  function redo() {
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
  }

  function worldPoint(evt) {
    const rect = canvas.getBoundingClientRect();
    return {
      x: (evt.clientX - rect.left - state.pan.x) / state.zoom,
      y: (evt.clientY - rect.top - state.pan.y) / state.zoom
    };
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

  function snapValue(value, grid = 8) {
    return state.snap ? Math.round(value / grid) * grid : value;
  }

  function selectedNodeKeys() {
    if (!state.selected) return [];
    if (state.selected.type === "node") return [state.selected.key];
    if (state.selected.type === "nodes") return state.selected.keys;
    return [];
  }

  function isNodeSelected(key) {
    return selectedNodeKeys().includes(key);
  }

  function toggleNodeSelection(key) {
    const keys = new Set(selectedNodeKeys());
    if (keys.has(key)) {
      keys.delete(key);
    } else {
      keys.add(key);
    }
    const next = [...keys];
    state.selected = next.length === 0 ? null : next.length === 1 ? { type: "node", key: next[0] } : { type: "nodes", keys: next };
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

  async function shareDiagram() {
    if (!state.current) return;
    const url = state.current.share_url || `${location.origin}/d/${state.current.slug}`;
    try {
      await navigator.clipboard.writeText(url);
      alert("Share link copied.");
    } catch {
      prompt("Share link", url);
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
      const group = svg("g", { class: selected ? "edge-selected" : "" }, edgesLayer);
      const d = `M ${a.x} ${a.y} L ${b.x} ${b.y}`;
      svg("path", { d, class: "edge-hit" }, group).addEventListener("mousedown", (evt) => {
        evt.stopPropagation();
        state.selected = { type: "edge", key: edge.key };
        render();
      });
      const path = svg("path", {
        d,
        class: "edge-path",
        stroke: edge.color || "#94a3b8",
        "marker-end": edge.directed ? "url(#arrow)" : ""
      }, group);
      path.addEventListener("mousedown", (evt) => {
        evt.stopPropagation();
        state.selected = { type: "edge", key: edge.key };
        render();
      });
      if (edge.label) {
        svg("text", { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 - 10, class: "edge-label" }, group).textContent = shortText(edge.label, 28);
      }
    }
  }

  function renderNodeShape(group, node) {
    const selected = isNodeSelected(node.key);
    group.setAttribute("class", selected ? "selected" : "");
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
  }

  function renderNodes() {
    nodesLayer.replaceChildren();
    if (!state.current) return;
    for (const node of state.current.nodes) {
      const group = svg("g", { "data-key": node.key }, nodesLayer);
      renderNodeShape(group, node);
      group.addEventListener("mousedown", (evt) => {
        evt.stopPropagation();
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
        if (evt.shiftKey && state.tool === "select") {
          toggleNodeSelection(node.key);
          render();
          return;
        }
        if (state.tool === "select") {
          pushHistory();
          const p = worldPoint(evt);
          const keys = selectedNodeKeys().includes(node.key) ? selectedNodeKeys() : [node.key];
          if (!selectedNodeKeys().includes(node.key)) {
            state.selected = { type: "node", key: node.key };
          }
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
    }
  }

  function renderProperties() {
    const empty = el("selection-empty");
    const nodeProps = el("node-props");
    const edgeProps = el("edge-props");
    nodeProps.classList.add("hidden");
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
    } else if (state.selected.type === "nodes") {
      empty.textContent = `${state.selected.keys.length} nodes selected`;
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
    renderEdges();
    renderNodes();
    renderProperties();
    renderMinimap();
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

  function renderDiagramList() {
    const list = el("diagram-list");
    list.replaceChildren();
    if (!state.diagrams.length) {
      const empty = document.createElement("div");
      empty.className = "muted";
      empty.textContent = "No diagrams yet.";
      list.appendChild(empty);
      return;
    }
    for (const d of state.diagrams) {
      const item = document.createElement("div");
      item.className = `diagram-item ${state.current?.id === d.id ? "active" : ""}`;
      item.innerHTML = `
        <div class="diagram-row">
          <div class="diagram-name"></div>
          <div class="small-actions">
            <button data-act="open">Open</button>
            <button data-act="dup">Copy</button>
            <button data-act="del" class="danger">Del</button>
          </div>
        </div>
        <div class="meta">${d.node_count || 0} nodes, ${d.edge_count || 0} edges</div>
        <div class="meta">Updated ${d.updated_at || ""}</div>`;
      item.querySelector(".diagram-name").textContent = d.title;
      item.querySelector('[data-act="open"]').onclick = () => openDiagram(d.id);
      item.querySelector('[data-act="dup"]').onclick = () => duplicateDiagram(d.id);
      item.querySelector('[data-act="del"]').onclick = () => deleteDiagram(d.id);
      list.appendChild(item);
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
      item.innerHTML = `
        <div class="diagram-row">
          <div class="diagram-name"></div>
          <div class="small-actions"><button data-act="use">Use</button></div>
        </div>
        <div class="meta"></div>
        <div class="meta"></div>`;
      item.querySelector(".diagram-name").textContent = template.title;
      item.querySelectorAll(".meta")[0].textContent = `${template.category || "Template"} - ${template.node_count || 0} nodes, ${template.edge_count || 0} edges`;
      item.querySelectorAll(".meta")[1].textContent = template.description || "";
      item.querySelector('[data-act="use"]').onclick = () => createFromTemplate(template.key);
      list.appendChild(item);
    }
  }

  async function loadDiagrams() {
    const data = await api("/api/diagrams");
    state.diagrams = data.diagrams || [];
    renderDiagramList();
    if (!state.current && !sharedSlugFromPath() && state.diagrams[0]) {
      await openDiagram(state.diagrams[0].id);
    }
  }

  async function loadTemplates() {
    const data = await api("/api/templates");
    state.templates = data.templates || [];
    renderTemplateList();
  }

  async function openDiagram(id) {
    state.current = await api(`/api/diagrams/${id}`);
    setReadOnly(state.current.can_edit === false);
    maybeRestoreDraft();
    state.history = [];
    state.future = [];
    state.selected = null;
    state.edgeSource = null;
    render();
    await loadVersions();
    renderDiagramList();
  }

  async function openDiagramBySlug(slug) {
    state.current = await api(`/api/diagrams/slug/${slug}`);
    setReadOnly(state.current.can_edit === false);
    maybeRestoreDraft();
    state.history = [];
    state.future = [];
    state.selected = null;
    state.edgeSource = null;
    render();
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
    const draftId = state.current.id;
    state.current.title = el("diagram-title").value.trim() || "Untitled diagram";
    state.current = await api(`/api/diagrams/${state.current.id}`, { method: "PUT", body: JSON.stringify(state.current) });
    clearDraft(draftId);
    state.history = [];
    state.future = [];
    await loadDiagrams();
    await loadVersions();
    render();
  }

  async function duplicateDiagram(id) {
    const created = await api(`/api/diagrams/${id}/duplicate`, { method: "POST", body: "{}" });
    await loadDiagrams();
    await openDiagram(created.id);
  }

  async function deleteDiagram(id) {
    if (state.readOnly && state.current?.id === id) return;
    if (!confirm("Delete this diagram?")) return;
    await api(`/api/diagrams/${id}`, { method: "DELETE" });
    if (state.current?.id === id) state.current = null;
    await loadDiagrams();
    render();
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
      item.innerHTML = `<strong>v${v.version_number}</strong><span class="meta">${v.created_at}</span><span class="meta"></span><button>Restore</button>`;
      item.querySelectorAll(".meta")[1].textContent = v.note || "";
      item.querySelector("button").onclick = async () => {
        if (!confirm(`Restore version ${v.version_number}?`)) return;
        if (state.readOnly) return;
        state.current = await api(`/api/diagrams/${state.current.id}/restore/${v.id}`, { method: "POST", body: "{}" });
        await loadVersions();
        render();
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
      x: snapValue(point.x, 16),
      y: snapValue(point.y, 16),
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
      const keys = selectedNodeKeys();
      state.current.nodes = state.current.nodes.filter((n) => !keys.includes(n.key));
      state.current.edges = state.current.edges.filter((e) => !keys.includes(e.source) && !keys.includes(e.target));
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
  }

  function copySelected() {
    if (!state.current || !state.selected) return;
    const keys = selectedNodeKeys();
    if (keys.length) {
      const keySet = new Set(keys);
      state.clipboard = {
        nodes: state.current.nodes.filter((n) => keySet.has(n.key)).map((n) => ({ ...n, metadata: { ...(n.metadata || {}) } })),
        edges: state.current.edges.filter((e) => keySet.has(e.source) && keySet.has(e.target)).map((e) => ({ ...e, metadata: { ...(e.metadata || {}) } }))
      };
    }
  }

  function pasteSelected() {
    if (!state.current || state.readOnly || !state.clipboard?.nodes?.length) return;
    pushHistory();
    const suffix = Date.now();
    const keyMap = new Map();
    const pastedKeys = [];
    for (const node of state.clipboard.nodes) {
      const key = `${node.key}_${suffix}`.slice(0, 64);
      keyMap.set(node.key, key);
      pastedKeys.push(key);
      state.current.nodes.push({ ...node, key, title: `${node.title} Copy`, x: node.x + 48, y: node.y + 48, metadata: { ...(node.metadata || {}) } });
    }
    for (const edge of state.clipboard.edges || []) {
      if (!keyMap.has(edge.source) || !keyMap.has(edge.target)) continue;
      state.current.edges.push({ ...edge, key: `${edge.key}_${suffix}`.slice(0, 64), source: keyMap.get(edge.source), target: keyMap.get(edge.target), metadata: { ...(edge.metadata || {}) } });
    }
    state.selected = pastedKeys.length === 1 ? { type: "node", key: pastedKeys[0] } : { type: "nodes", keys: pastedKeys };
    render();
  }

  function autoLayout() {
    if (!state.current || state.readOnly) return;
    pushHistory();
    const nodes = state.current.nodes;
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
        node.x = 80 + l * 260;
        node.y = 80 + i * 140;
      });
    });
    render();
  }

  function bindProperty(id, apply) {
    el(id).addEventListener("change", () => {
      if (!state.current || !state.selected || state.readOnly) return;
      pushHistory();
      apply();
      render();
    });
  }

  function initBindings() {
    nodeTypes.forEach((type) => {
      const opt = document.createElement("option");
      opt.value = type;
      opt.textContent = type;
      el("prop-type").appendChild(opt);
    });
    document.querySelectorAll(".toolbox button[data-tool]").forEach((btn) => btn.onclick = () => {
      setTool(btn.dataset.tool);
      if (btn.dataset.tool === "node" && state.current) {
        createNodeAt(viewportCenterPoint());
      }
    });
    el("new-diagram").onclick = newDiagram;
    el("load-sample").onclick = loadSample;
    el("share-diagram").onclick = shareDiagram;
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
    el("zoom-in").onclick = () => { state.zoom = Math.min(2.5, state.zoom * 1.15); render(); };
    el("zoom-out").onclick = () => { state.zoom = Math.max(0.25, state.zoom / 1.15); render(); };
    el("export-json").onclick = () => { if (state.current) window.open(`/api/diagrams/${state.current.id}/export.json`, "_blank"); };
    el("export-svg").onclick = exportSvg;
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
        alert("Import file is too large.");
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
    bindProperty("prop-width", () => state.current.nodes.find((n) => n.key === state.selected.key).width = Number(el("prop-width").value));
    bindProperty("prop-height", () => state.current.nodes.find((n) => n.key === state.selected.key).height = Number(el("prop-height").value));
    bindProperty("edge-label", () => state.current.edges.find((e) => e.key === state.selected.key).label = el("edge-label").value);
    bindProperty("edge-color", () => state.current.edges.find((e) => e.key === state.selected.key).color = el("edge-color").value);
    bindProperty("edge-directed", () => state.current.edges.find((e) => e.key === state.selected.key).directed = el("edge-directed").checked);

    canvas.addEventListener("mousedown", (evt) => {
      if (evt.button === 1 || state.tool === "pan") {
        state.panning = { x: evt.clientX - state.pan.x, y: evt.clientY - state.pan.y };
        return;
      }
      if (state.tool === "node") {
        createNodeAt(worldPoint(evt));
      } else {
        state.selected = null;
        state.edgeSource = null;
        render();
      }
    });

    window.addEventListener("mousemove", (evt) => {
      if (state.dragging && state.current) {
        if (state.readOnly) return;
        const node = state.current.nodes.find((n) => n.key === state.dragging.key);
        if (node) {
          const p = worldPoint(evt);
          const nextX = snapValue(p.x - state.dragging.dx);
          const nextY = snapValue(p.y - state.dragging.dy);
          const deltaX = nextX - state.dragging.originals[state.dragging.key].x;
          const deltaY = nextY - state.dragging.originals[state.dragging.key].y;
          for (const key of state.dragging.keys || [state.dragging.key]) {
            const moving = state.current.nodes.find((n) => n.key === key);
            const original = state.dragging.originals[key];
            if (moving && original) {
              moving.x = original.x + deltaX;
              moving.y = original.y + deltaY;
            }
          }
          render();
        }
      } else if (state.panning) {
        state.pan.x = evt.clientX - state.panning.x;
        state.pan.y = evt.clientY - state.panning.y;
        render();
      }
    });

    window.addEventListener("mouseup", () => {
      if (state.dragging && state.current && !state.readOnly) markDirty();
      state.dragging = null;
      state.panning = null;
    });

    canvas.addEventListener("wheel", (evt) => {
      evt.preventDefault();
      const before = worldPoint(evt);
      state.zoom = Math.max(0.25, Math.min(2.5, state.zoom * (evt.deltaY < 0 ? 1.08 : 0.92)));
      const rect = canvas.getBoundingClientRect();
      state.pan.x = evt.clientX - rect.left - before.x * state.zoom;
      state.pan.y = evt.clientY - rect.top - before.y * state.zoom;
      render();
    }, { passive: false });

    window.addEventListener("keydown", (evt) => {
      if (evt.target.matches("input, textarea, select")) return;
      if (evt.key === "Delete" || evt.key === "Backspace") deleteSelected();
      if (evt.key === "Escape") { state.selected = null; state.edgeSource = null; render(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "s") { evt.preventDefault(); saveDiagram(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "c") { evt.preventDefault(); copySelected(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "v") { evt.preventDefault(); pasteSelected(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "z") { evt.preventDefault(); evt.shiftKey ? redo() : undo(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "y") { evt.preventDefault(); redo(); }
    });

    window.addEventListener("beforeunload", (evt) => {
      if (!state.dirty) return;
      evt.preventDefault();
      evt.returnValue = "";
    });
  }

  initBindings();
  Promise.all([loadTemplates(), loadDiagrams()])
    .then(() => {
      const slug = sharedSlugFromPath();
      if (slug) return openDiagramBySlug(slug);
    })
    .catch((err) => alert(err.message));
  render();
})();
