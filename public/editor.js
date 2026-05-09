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
    current: null,
    selected: null,
    tool: "select",
    zoom: 1,
    pan: { x: 80, y: 70 },
    edgeSource: null,
    history: [],
    future: [],
    dragging: null,
    panning: null
  };

  const el = (id) => document.getElementById(id);
  const canvas = el("canvas");
  const viewport = el("viewport");
  const nodesLayer = el("nodes");
  const edgesLayer = el("edges");

  function api(path, options = {}) {
    return fetch(path, {
      credentials: "same-origin",
      headers: { "Content-Type": "application/json", ...(options.headers || {}) },
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
    if (!state.current) return;
    state.history.push(cloneDiagram());
    if (state.history.length > 80) state.history.shift();
    state.future = [];
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
  }

  function redo() {
    if (!state.future.length || !state.current) return;
    state.history.push(cloneDiagram());
    restoreSnapshot(state.future.pop());
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
    const selected = state.selected?.type === "node" && state.selected.key === node.key;
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
        state.selected = { type: "node", key: node.key };
        if (state.tool === "select") {
          pushHistory();
          const p = worldPoint(evt);
          state.dragging = { key: node.key, dx: p.x - node.x, dy: p.y - node.y };
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
      el("diagram-title").value = state.current.title;
    }
    renderEdges();
    renderNodes();
    renderProperties();
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

  async function loadDiagrams() {
    const data = await api("/api/diagrams");
    state.diagrams = data.diagrams || [];
    renderDiagramList();
    if (!state.current && state.diagrams[0]) {
      await openDiagram(state.diagrams[0].id);
    }
  }

  async function openDiagram(id) {
    state.current = await api(`/api/diagrams/${id}`);
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

  async function saveDiagram() {
    if (!state.current) return;
    state.current.title = el("diagram-title").value.trim() || "Untitled diagram";
    state.current = await api(`/api/diagrams/${state.current.id}`, { method: "PUT", body: JSON.stringify(state.current) });
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
    const data = await api(`/api/diagrams/${state.current.id}/versions`);
    for (const v of data.versions || []) {
      const item = document.createElement("div");
      item.className = "version-item";
      item.innerHTML = `<strong>v${v.version_number}</strong><span class="meta">${v.created_at}</span><span class="meta"></span><button>Restore</button>`;
      item.querySelectorAll(".meta")[1].textContent = v.note || "";
      item.querySelector("button").onclick = async () => {
        if (!confirm(`Restore version ${v.version_number}?`)) return;
        state.current = await api(`/api/diagrams/${state.current.id}/restore/${v.id}`, { method: "POST", body: "{}" });
        await loadVersions();
        render();
      };
      list.appendChild(item);
    }
  }

  function createNodeAt(point) {
    if (!state.current) return;
    pushHistory();
    const type = el("node-type").value;
    const size = type === "decision" ? { width: 150, height: 96 } : { width: 170, height: 82 };
    state.current.nodes.push({
      key: `node_${Date.now()}`,
      type,
      title: type.charAt(0).toUpperCase() + type.slice(1),
      x: Math.round(point.x / 16) * 16,
      y: Math.round(point.y / 16) * 16,
      width: size.width,
      height: size.height,
      color: colors[type] || colors.process,
      metadata: {}
    });
    state.selected = { type: "node", key: state.current.nodes[state.current.nodes.length - 1].key };
    render();
  }

  function deleteSelected() {
    if (!state.current || !state.selected) return;
    pushHistory();
    if (state.selected.type === "node") {
      const key = state.selected.key;
      state.current.nodes = state.current.nodes.filter((n) => n.key !== key);
      state.current.edges = state.current.edges.filter((e) => e.source !== key && e.target !== key);
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

  function bindProperty(id, apply) {
    el(id).addEventListener("change", () => {
      if (!state.current || !state.selected) return;
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
    el("save-diagram").onclick = saveDiagram;
    el("refresh-diagrams").onclick = loadDiagrams;
    el("delete-selected").onclick = deleteSelected;
    el("undo").onclick = undo;
    el("redo").onclick = redo;
    el("zoom-in").onclick = () => { state.zoom = Math.min(2.5, state.zoom * 1.15); render(); };
    el("zoom-out").onclick = () => { state.zoom = Math.max(0.25, state.zoom / 1.15); render(); };
    el("export-json").onclick = () => { if (state.current) window.open(`/api/diagrams/${state.current.id}/export.json`, "_blank"); };
    el("make-version").onclick = async () => {
      if (!state.current) return;
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
      if (!state.current) return;
      pushHistory();
      state.current.title = el("diagram-title").value.trim() || "Untitled diagram";
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
        const node = state.current.nodes.find((n) => n.key === state.dragging.key);
        if (node) {
          const p = worldPoint(evt);
          node.x = Math.round((p.x - state.dragging.dx) / 8) * 8;
          node.y = Math.round((p.y - state.dragging.dy) / 8) * 8;
          render();
        }
      } else if (state.panning) {
        state.pan.x = evt.clientX - state.panning.x;
        state.pan.y = evt.clientY - state.panning.y;
        render();
      }
    });

    window.addEventListener("mouseup", () => {
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
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "z") { evt.preventDefault(); evt.shiftKey ? redo() : undo(); }
      if ((evt.ctrlKey || evt.metaKey) && evt.key.toLowerCase() === "y") { evt.preventDefault(); redo(); }
    });
  }

  initBindings();
  loadDiagrams().catch((err) => alert(err.message));
  render();
})();
