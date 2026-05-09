#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"

namespace nuigraph {

std::string renderLoginPage(const std::string& error) {
    return std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NuiGraph Studio Login</title>
  <link rel="stylesheet" href="/styles.css">
</head>
<body class="login-body">
  <main class="login-shell">
    <form class="login-panel" method="post" action="/login">
      <div class="brand-mark">NG</div>
      <h1>NuiGraph Studio</h1>
      <p>Admin access is required for diagram editing.</p>
)HTML") + (error.empty() ? "" : "<div class=\"login-error\">" + htmlEscape(error) + "</div>") + R"HTML(
      <label>Username<input name="username" autocomplete="username" required maxlength="80"></label>
      <label>Password<input type="password" name="password" autocomplete="current-password" required maxlength="200"></label>
      <button type="submit">Sign in</button>
      <a href="/health">Health check</a>
    </form>
  </main>
</body>
</html>)HTML";
}

std::string renderEditorPage(const std::string& username) {
    return std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NuiGraph Studio</title>
  <link rel="stylesheet" href="/styles.css">
</head>
<body>
  <div id="app" data-user=")HTML") + htmlEscape(username) + R"HTML(">
    <header class="topbar">
      <div class="app-title">
        <strong>NuiGraph Studio</strong>
        <input id="diagram-title" maxlength="120" value="Untitled diagram" aria-label="Diagram name">
      </div>
      <div class="top-actions">
        <button id="new-diagram" title="New diagram">New</button>
        <button id="save-diagram" title="Save">Save</button>
        <button id="undo" title="Undo">Undo</button>
        <button id="redo" title="Redo">Redo</button>
        <button id="export-json" title="Export JSON">Export</button>
        <button id="import-json" title="Import JSON">Import</button>
        <button id="zoom-out" title="Zoom out">-</button>
        <span id="zoom-label">100%</span>
        <button id="zoom-in" title="Zoom in">+</button>
        <a href="/docs">Docs</a>
        <form method="post" action="/logout"><button type="submit">Logout</button></form>
      </div>
    </header>
    <aside class="project-panel">
      <div class="panel-head">
        <h2>Diagrams</h2>
        <button id="refresh-diagrams" title="Refresh">Refresh</button>
      </div>
      <div id="diagram-list" class="diagram-list"></div>
    </aside>
    <nav class="toolbox">
      <button data-tool="select" class="active">Select</button>
      <button data-tool="pan">Pan</button>
      <button data-tool="node">Node</button>
      <button data-tool="edge">Edge</button>
      <button id="delete-selected">Delete</button>
      <select id="node-type" aria-label="Node type">
        <option value="process">Process</option>
        <option value="decision">Decision</option>
        <option value="database">Database</option>
        <option value="service">Service</option>
        <option value="api">API</option>
        <option value="note">Note</option>
        <option value="external">External</option>
      </select>
    </nav>
    <main class="canvas-wrap">
      <svg id="canvas" role="application" aria-label="Diagram canvas">
        <defs>
          <pattern id="grid" width="32" height="32" patternUnits="userSpaceOnUse">
            <path d="M 32 0 L 0 0 0 32" fill="none" stroke="rgba(148,163,184,.18)" stroke-width="1"/>
          </pattern>
          <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse">
            <path d="M 0 0 L 10 5 L 0 10 z" fill="#94a3b8"></path>
          </marker>
        </defs>
        <rect id="grid-bg" x="-50000" y="-50000" width="100000" height="100000" fill="url(#grid)"></rect>
        <g id="viewport"><g id="edges"></g><g id="nodes"></g></g>
      </svg>
    </main>
    <aside class="properties">
      <h2>Properties</h2>
      <div id="selection-empty" class="muted">No selection</div>
      <div id="node-props" class="prop-block hidden">
        <label>Title<input id="prop-title" maxlength="160"></label>
        <label>Type<select id="prop-type"></select></label>
        <label>Color<input id="prop-color" type="color"></label>
        <label>Width<input id="prop-width" type="number" min="72" max="600"></label>
        <label>Height<input id="prop-height" type="number" min="48" max="400"></label>
      </div>
      <div id="edge-props" class="prop-block hidden">
        <label>Label<input id="edge-label" maxlength="160"></label>
        <label>Color<input id="edge-color" type="color"></label>
        <label class="check-row"><input id="edge-directed" type="checkbox"> Directed</label>
      </div>
      <section class="versions">
        <div class="panel-head">
          <h2>Versions</h2>
          <button id="make-version">Snapshot</button>
        </div>
        <div id="version-list" class="version-list"></div>
      </section>
    </aside>
  </div>
  <input id="import-file" type="file" accept="application/json,.json" hidden>
  <script src="/app.js"></script>
</body>
</html>)HTML";
}

std::string renderDocsPage() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NuiGraph Studio Docs</title>
  <link rel="stylesheet" href="/styles.css">
</head>
<body class="docs-body">
  <main class="docs">
    <a href="/">Back to editor</a>
    <h1>NuiGraph Studio</h1>
    <p>NuiGraph Studio is a C++ powered graph and diagram editor for flowcharts, architecture maps, service graphs, and node-link diagrams.</p>
    <h2>Canvas Controls</h2>
    <p>Select nodes and edges with the Select tool. Drag nodes to move them. Use the Node tool to create nodes, Edge to connect two nodes, and Pan or middle mouse drag to move the canvas. Delete removes the selected object. Ctrl+S saves, Ctrl+Z/Ctrl+Y undo and redo, Esc clears selection.</p>
    <h2>Node Types</h2>
    <p>Supported types: process, decision, database, service, api, note, external. Unknown imported types are mapped to note.</p>
    <h2>Edges</h2>
    <p>Edges may be directed or undirected, labeled, and colored. Validation rejects edges that reference missing nodes.</p>
    <h2>JSON Format</h2>
    <pre>{
  "title": "Example",
  "description": "",
  "nodes": [{"key":"node_1","type":"process","title":"Build","x":120,"y":80,"width":160,"height":80,"color":"#38bdf8","metadata":{}}],
  "edges": [{"key":"edge_1","source":"node_1","target":"node_2","label":"calls","directed":true,"color":"#94a3b8","metadata":{}}]
}</pre>
    <h2>API</h2>
    <pre>GET /health
GET /api/diagrams
POST /api/diagrams
GET /api/diagrams/{id}
PUT /api/diagrams/{id}
DELETE /api/diagrams/{id}
POST /api/diagrams/{id}/duplicate
GET /api/diagrams/{id}/versions
POST /api/diagrams/{id}/versions
POST /api/diagrams/{id}/restore/{versionId}
GET /api/diagrams/{id}/export.json
POST /api/diagrams/import</pre>
    <h2>Storage And Versioning</h2>
    <p>PostgreSQL stores diagrams, nodes, edges, and bounded version snapshots. Each save creates a snapshot and old versions are pruned by MAX_VERSIONS_PER_DIAGRAM.</p>
    <h2>Limitations</h2>
    <p>v1 is single-user at a time with immediate browser updates, not WebSocket collaboration. The authenticated editor shell is rendered by Nui C++ compiled with Emscripten/WASM; detailed canvas gestures use a small JavaScript bridge.</p>
  </main>
</body>
</html>)HTML";
}

} // namespace nuigraph
