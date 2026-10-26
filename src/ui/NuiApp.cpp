#include "ui/NuiApp.hpp"
#include "utils/Json.hpp"

namespace nuigraph {
namespace {

constexpr const char* kDescription =
    "Create flowcharts, architecture maps, service graphs and node-link diagrams in your browser.";

std::string normalizedBaseUrl(std::string baseUrl) {
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }
    return baseUrl;
}

std::string assetUrl(const std::string& path, const std::string& assetVersion) {
    if (assetVersion.empty()) {
        return path;
    }
    return path + "?v=" + htmlEscape(assetVersion);
}

std::string publicMetadata(const std::string& title, const std::string& canonicalUrl,
                           bool indexable, bool softwareApplication = false) {
    std::string metadata =
        "  <meta name=\"description\" content=\"" + std::string(kDescription) + "\">\n" +
        "  <meta name=\"robots\" content=\"" +
        (indexable ? "index, follow" : "noindex, nofollow, noarchive") + "\">\n";
    if (!indexable) {
        return metadata;
    }

    const auto escapedTitle = htmlEscape(title);
    const auto escapedUrl = htmlEscape(canonicalUrl);
    metadata +=
        "  <link rel=\"canonical\" href=\"" + escapedUrl + "\">\n"
        "  <meta property=\"og:type\" content=\"website\">\n"
        "  <meta property=\"og:site_name\" content=\"NuiGraph Studio\">\n"
        "  <meta property=\"og:locale\" content=\"en_US\">\n"
        "  <meta property=\"og:title\" content=\"" + escapedTitle + "\">\n"
        "  <meta property=\"og:description\" content=\"" + std::string(kDescription) + "\">\n"
        "  <meta property=\"og:url\" content=\"" + escapedUrl + "\">\n"
        "  <meta name=\"twitter:card\" content=\"summary\">\n"
        "  <meta name=\"twitter:title\" content=\"" + escapedTitle + "\">\n"
        "  <meta name=\"twitter:description\" content=\"" + std::string(kDescription) + "\">\n"
        "  <link rel=\"alternate\" hreflang=\"en\" href=\"" + escapedUrl + "\">\n"
        "  <link rel=\"alternate\" hreflang=\"x-default\" href=\"" + escapedUrl + "\">\n";
    if (softwareApplication) {
        metadata +=
            "  <meta itemprop=\"name\" content=\"NuiGraph Studio\">\n"
            "  <meta itemprop=\"description\" content=\"" + std::string(kDescription) + "\">\n"
            "  <meta itemprop=\"applicationCategory\" content=\"DesignApplication\">\n"
            "  <meta itemprop=\"operatingSystem\" content=\"Any modern browser\">\n"
            "  <meta itemprop=\"isAccessibleForFree\" content=\"true\">\n"
            "  <link itemprop=\"url\" href=\"" + escapedUrl + "\">\n";
    }
    return metadata;
}

} // namespace

std::string renderLoginPage(const std::string& error, const std::string& assetVersion) {
    return std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="description" content="Administrator sign-in for NuiGraph Studio.">
  <meta name="robots" content="noindex, nofollow, noarchive">
  <title>NuiGraph Studio Login</title>
  <link rel="stylesheet" href=")HTML" + assetUrl("/styles.css", assetVersion) + R"HTML(">
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

std::string renderEditorPage(const std::string& username, const std::string& baseUrl,
                             bool indexable, const std::string& assetVersion) {
    const auto canonicalUrl = normalizedBaseUrl(baseUrl) + "/";
    return std::string("<!doctype html>\n<html lang=\"en\"") +
        (indexable ? " itemscope itemtype=\"https://schema.org/SoftwareApplication\"" : "") + R"HTML(>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>NuiGraph Studio — Browser Diagram Editor</title>
)HTML" + publicMetadata("NuiGraph Studio — Browser Diagram Editor", canonicalUrl, indexable, indexable) + R"HTML(  <meta name="theme-color" content="#0b0f14">
  <link rel="stylesheet" href=")HTML" + assetUrl("/styles.css", assetVersion) + R"HTML(">
  <link rel="manifest" href=")HTML" + assetUrl("/manifest.webmanifest", assetVersion) + R"HTML(">
  <link rel="icon" href=")HTML" + assetUrl("/icons/icon.svg", assetVersion) + R"HTML(" type="image/svg+xml">
  <link rel="apple-touch-icon" href=")HTML" + assetUrl("/icons/icon.svg", assetVersion) + R"HTML(">
</head>
<body>
  <div id="app" data-user=")HTML" + htmlEscape(username) + R"HTML(">
    <header class="topbar">
      <div class="app-title">
        <h1>NuiGraph Studio</h1>
        <input id="diagram-title" maxlength="120" value="Untitled diagram" aria-label="Diagram name">
      </div>
      <div class="top-actions">
        <button id="new-diagram" title="New diagram">New</button>
        <button id="load-sample" title="Load sample architecture">Sample</button>
        <button id="share-diagram" title="Manage sharing">Share</button>
        <button id="save-diagram" title="Save">Save</button>
        <button id="undo" title="Undo">Undo</button>
        <button id="redo" title="Redo">Redo</button>
        <button id="copy-selected" title="Copy selected">Copy</button>
        <button id="paste-selected" title="Paste copied">Paste</button>
        <button id="auto-layout" title="Auto layout">Layout</button>
        <button id="snap-toggle" title="Toggle snap to grid" class="active">Snap</button>
        <button id="export-json" title="Export JSON">Export</button>
        <button id="export-svg" title="Export SVG">SVG</button>
        <button id="export-png" title="Export PNG">PNG</button>
        <button id="import-json" title="Import JSON">Import</button>
        <button id="zoom-out" title="Zoom out">-</button>
        <button id="zoom-label" title="Actual size (Shift+1)" aria-label="100% zoom; reset to 100%">100%</button>
        <button id="zoom-in" title="Zoom in">+</button>
        <span id="draft-status" class="draft-status">Saved</span>
        <a href="/docs">Docs</a>
        <a href="/privacy">Privacy</a>
        <a href="/login">Admin</a>
      </div>
    </header>
    <aside class="project-panel">
      <div class="panel-head">
        <h2>Diagrams</h2>
        <div class="panel-actions">
          <button id="privacy-data" title="Privacy and data controls">Data</button>
          <button id="open-trash" title="Open trash">Trash</button>
          <button id="refresh-diagrams" title="Refresh">Refresh</button>
        </div>
      </div>
      <div id="diagram-list" class="diagram-list"></div>
      <section class="template-section">
        <div class="panel-head"><h2>Templates</h2></div>
        <div id="template-list" class="diagram-list"></div>
      </section>
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
        <option value="queue">Queue</option>
        <option value="cache">Cache</option>
        <option value="container">Container</option>
        <option value="cloud">Cloud</option>
      </select>
      <select id="grid-size" aria-label="Grid size">
        <option value="8">Grid 8</option>
        <option value="16" selected>Grid 16</option>
        <option value="32">Grid 32</option>
        <option value="64">Grid 64</option>
      </select>
    </nav>
    <main class="canvas-wrap">
      <svg id="canvas" role="application" aria-label="Diagram canvas">
        <defs>
          <pattern id="grid" width="16" height="16" patternUnits="userSpaceOnUse">
            <path d="M 16 0 L 0 0 0 16" fill="none" stroke="rgba(148,163,184,.18)" stroke-width="1"/>
          </pattern>
          <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="8" markerHeight="8" orient="auto-start-reverse">
            <path d="M 0 0 L 10 5 L 0 10 z" fill="#94a3b8"></path>
          </marker>
        </defs>
        <rect id="grid-bg" x="-50000" y="-50000" width="100000" height="100000" fill="url(#grid)"></rect>
        <g id="viewport"><g id="edges"></g><g id="nodes"></g><g id="overlay"></g></g>
      </svg>
      <div id="minimap" class="minimap"></div>
    </main>
    <aside class="properties">
      <h2>Properties</h2>
      <div id="selection-empty" class="muted">No selection</div>
      <div id="node-props" class="prop-block hidden">
        <label>Title<input id="prop-title" maxlength="160"></label>
        <label>Type<select id="prop-type"></select></label>
        <label>Trust zone<select id="prop-trust-zone"><option value="">Unspecified</option><option value="internet">Internet</option><option value="edge">Edge</option><option value="application">Application</option><option value="data">Data</option><option value="management">Management</option></select></label>
        <label>Color<input id="prop-color" type="color"></label>
        <label>Width<input id="prop-width" type="number" min="72" max="600"></label>
        <label>Height<input id="prop-height" type="number" min="48" max="400"></label>
      </div>
      <div id="multi-node-props" class="prop-block hidden">
        <div class="selection-summary">
          <strong id="multi-node-count">Multiple nodes selected</strong>
          <span id="multi-node-summary" class="muted"></span>
        </div>
        <label>Type<select id="batch-type"><option value="">Keep current types</option></select></label>
        <label class="batch-property"><span class="check-row"><input id="batch-color-enabled" type="checkbox"> Set color</span><input id="batch-color" type="color" value="#38bdf8" disabled></label>
        <label class="batch-property"><span class="check-row"><input id="batch-width-enabled" type="checkbox"> Set width</span><input id="batch-width" type="number" min="72" max="600" value="170" disabled></label>
        <label class="batch-property"><span class="check-row"><input id="batch-height-enabled" type="checkbox"> Set height</span><input id="batch-height" type="number" min="48" max="400" value="82" disabled></label>
        <button id="apply-batch-properties" class="primary" type="button">Apply to selected nodes</button>
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
  <script src=")HTML" + assetUrl("/app.js", assetVersion) + R"HTML("></script>
</body>
</html>)HTML";
}

std::string renderDocsPage(const std::string& baseUrl, const std::string& assetVersion) {
    const auto canonicalUrl = normalizedBaseUrl(baseUrl) + "/docs";
    return std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Documentation | NuiGraph Studio</title>
)HTML") + publicMetadata("NuiGraph Studio Documentation", canonicalUrl, true) + R"HTML(  <link rel="stylesheet" href=")HTML" + assetUrl("/styles.css", assetVersion) + R"HTML(">
</head>
<body class="docs-body">
  <main class="docs">
    <a href="/">Back to editor</a>
    <h1>NuiGraph Studio</h1>
    <p>NuiGraph Studio is a C++ powered graph and diagram editor for flowcharts, architecture maps, service graphs, and node-link diagrams. Diagrams start private. Owners can enable an unguessable read-only link, rotate it, or disable it immediately; visitors can fork an active shared diagram into their own session.</p>
    <h2>Canvas Controls</h2>
    <p>Select nodes and edges with the Select tool. Shift-click nodes for basic multi-select, or drag on empty canvas to rubber-band everything the band touches; hold Shift while dragging to add to the current selection instead of replacing it. Drag nodes to move them; edges and centres latch onto nearby nodes and a guide line shows what lined up. Choose an 8, 16, 32 or 64 pixel grid to change both the visual spacing and snap interval. Adaptive coordinate rulers follow pan and zoom; toggle them from Commands or a pinned mobile action. Arrow keys move selected nodes by one grid step and Shift+Arrow moves five; key repeat is grouped into one undo. Multi-node selections can align left, centre, right, top, middle or bottom, and three or more nodes can distribute equal centre spacing through the command palette. The multi-selection inspector summarises type, colour and bounds, then applies shared type, colour or size changes as one undoable edit. Press Shift+1 for actual size (100%), Shift+2 to frame the selected nodes or both endpoints of a selected edge, Shift+P for the edit-safe full-viewport presentation mode, or Ctrl/Cmd+P for the clean A4 landscape print/PDF layout. A single selected node gets eight resize handles; the opposite edge stays anchored and sizes are held between 72x48 and 600x400, the same range the server enforces. Double-click a node or edge, or select it and press F2 or Enter, to edit its label directly on the canvas; the same action can be pinned on mobile. Use the Node tool to create nodes, Edge to connect two nodes, and Pan or middle mouse drag to move the canvas. Delete removes the selected object. Press / to search nodes by title, type, trust zone or key and centre a result. Ctrl+K (or Cmd+K) opens the searchable command palette; Ctrl+S saves, Ctrl+C/Ctrl+V copy and paste selected nodes, Ctrl+Z/Ctrl+Y undo and redo, and Esc closes transient UI or clears selection. Actual size, Zoom to fit, Snap, Layout, viewport/embeddable/selection SVG, opaque/transparent PNG, cropped selection PNG, Mermaid and Sample are available from the palette. Diagram Review checks structure locally in the browser for orphan nodes, invalid or duplicate edges, missing titles or labels, self-loops and directed cycles. Once a node is assigned an internet, edge, application, data or management trust zone, the same review flags incomplete classification, unlabelled boundary crossings and direct internet-to-data relationships; Focus centres the affected object and SARIF exports the findings for CI. On phones a compact bottom bar keeps the canvas large; choose Customize mobile action bar from the palette to pin up to two preferred actions. Unsaved browser drafts are stored locally and can be restored when reopening the same diagram.</p>
    <h2>Node Types</h2>
    <p>Supported types: process, decision, database, service, api, note, external, queue, cache, container, and cloud. Unknown imported types are mapped to note. Optional trust zones are server-validated and shown in the node subtitle.</p>
    <h2>Edges</h2>
    <p>Edges may be directed or undirected, labeled, and colored. Validation rejects edges that reference missing nodes.</p>
    <h2>JSON Format</h2>
    <pre>{
  "title": "Example",
  "description": "",
  "nodes": [{"key":"node_1","type":"process","title":"Build","x":120,"y":80,"width":160,"height":80,"color":"#38bdf8","metadata":{"trust_zone":"application"}}],
  "edges": [{"key":"edge_1","source":"node_1","target":"node_2","label":"calls","directed":true,"color":"#94a3b8","metadata":{}}]
}</pre>
    <h2>API</h2>
    <p><a href="/openapi.json">OpenAPI 3.1 contract (JSON)</a> documents request schemas, session/CSRF requirements, optimistic revision headers and response models.</p>
    <pre>GET /health
GET /live
GET /ready
POST /api/privacy/export
DELETE /api/privacy/data
GET /api/templates
POST /api/templates/{key}/create
GET /api/diagrams
POST /api/diagrams
GET /api/diagrams/{id}
GET /api/diagrams/slug/{slug}
POST /api/diagrams/slug/{slug}/fork
POST /api/diagrams/{id}/sharing
PUT /api/diagrams/{id}
DELETE /api/diagrams/{id}
POST /api/diagrams/{id}/duplicate
GET /api/diagrams/{id}/versions
POST /api/diagrams/{id}/versions
GET /api/diagrams/{id}/versions/{versionId}
POST /api/diagrams/{id}/restore/{versionId}
GET /api/diagrams/{id}/export.json
POST /api/diagrams/import</pre>
    <h2>Storage And Versioning</h2>
    <p>PostgreSQL stores diagrams, nodes, edges, and bounded version snapshots. Each save creates a snapshot and old versions are pruned by MAX_VERSIONS_PER_DIAGRAM. Saves and restores use revision ETags, so a stale tab is rejected instead of overwriting newer work; its browser draft remains available. Diff on a version lists what was added, removed and changed between that snapshot and the diagram currently open in the editor, including unsaved edits.</p>
    <h2>Privacy And Data Controls</h2>
    <p>The editor can export active diagrams, trash and retained versions owned by the current guest session, or permanently delete that session's diagrams after an exact typed confirmation. These controls are cookie-bound and do not give the administrator a bulk owner operation. See the <a href="/privacy">privacy notice</a> for retention, logging and infrastructure limitations.</p>
    <h2>Security</h2>
    <p>Public editing uses signed guest sessions, CSRF tokens, owner checks, input validation, and IP-based write/create rate limits. Admin login remains available for full access.</p>
    <h2>Templates</h2>
    <p>The app includes built-in cloud architecture, incident-response, and data-pipeline templates. Creating from a template stores a normal versioned diagram owned by the current guest session or admin.</p>
    <h2>Limitations</h2>
    <p>v1 is not WebSocket collaboration: concurrent saves are detected but require manual reload and draft reconciliation. Guest ownership is session based rather than full account identity. Production uses the server-rendered C++ shell; an explicitly selected Nui/Emscripten shell is available only when a complete WASM bundle passes startup validation. Detailed canvas gestures use a small JavaScript bridge. Mutating API calls require CSRF tokens.</p>
  </main>
</body>
</html>)HTML";
}

std::string renderPrivacyPage(const std::string& baseUrl, int trashRetentionDays,
                              const std::string& assetVersion) {
    const auto canonicalUrl = normalizedBaseUrl(baseUrl) + "/privacy";
    const auto trashPolicy = trashRetentionDays > 0
        ? "Trash becomes eligible for bounded automatic deletion after " +
              std::to_string(trashRetentionDays) + " days."
        : "Automatic trash deletion is disabled; trash remains until the owning session permanently deletes it.";
    return std::string(R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Privacy Notice | NuiGraph Studio</title>
)HTML") + publicMetadata("NuiGraph Studio Privacy Notice", canonicalUrl, true) + R"HTML(  <link rel="stylesheet" href=")HTML" + assetUrl("/styles.css", assetVersion) + R"HTML(">
</head>
<body class="docs-body">
  <main class="docs">
    <a href="/">Back to editor</a>
    <h1>Privacy notice</h1>
    <p><strong>Last updated:</strong> 27 October 2026. This notice describes the repository's current application behavior; it does not claim that every external infrastructure or legal obligation is fully documented.</p>
    <h2>Controller and contact</h2>
    <p>NuiGraph Studio is operated by the owner of this deployment. Privacy and data-rights questions can be sent to <a href="mailto:alex_mihai984@yahoo.com">alex_mihai984@yahoo.com</a>.</p>
    <h2>Data processed</h2>
    <ul>
      <li>A signed, pseudonymous guest-session cookie used to keep diagrams attached to the same browser session. The cookie expires after seven days; clearing it can make guest-owned diagrams inaccessible.</li>
      <li>Diagram titles, descriptions, nodes, edges, metadata, retained versions, trash state, and optional bearer share links supplied through the editor.</li>
      <li>Request metadata needed for security and operations, including IP-derived rate-limit identifiers, timestamps, status classes, and redacted application/access logs. Metrics contain aggregate bounded labels rather than diagram content or owner identifiers.</li>
      <li>Administrator authentication data configured by the operator. The guest self-service export does not export administrator configuration or other owners' records.</li>
    </ul>
    <h2>Purposes and legal context</h2>
    <p>Diagram and session data are processed to provide the storage, editing, sharing, recovery, export, and deletion functions requested by the visitor. Limited request and security data are processed to protect the service, enforce quotas, diagnose failures, and investigate abuse. The operator must assess and document the applicable legal basis for its deployment; this application does not use diagram content for advertising, profiling, or automated decisions with legal or similarly significant effects.</p>
    <h2>Sharing and recipients</h2>
    <p>Diagrams are private by default. Enabling sharing creates an unguessable read-only bearer URL: anyone who receives that URL can view the diagram until the owner disables, rotates, expires, trashes, or deletes it. Infrastructure recipients include Cloudflare as the public reverse proxy and the operator's VPS/hosting and PostgreSQL environment. Contact the operator for the current provider, location, transfer safeguards, and subprocessor details before relying on this page as a complete Article 13 notice.</p>
    <h2>Retention</h2>
    <p>Active diagrams and bounded versions remain while the guest session retains them. Moving a diagram to trash revokes sharing but does not erase or release its quota. )HTML" + htmlEscape(trashPolicy) + R"HTML( Permanent deletion and “Delete my session data” remove primary PostgreSQL rows and cascaded nodes, edges, versions, and idempotency records. Those actions do not by themselves prove erasure from separately retained logs or backup media. Rate-limit rows expire within ten minutes and idempotency records within 24 hours. A fixed external log-retention period remains an operator control; request the current period from the contact above.</p>
    <h2>Your controls and rights</h2>
    <p>The editor's Privacy &amp; data dialog can download a machine-readable JSON copy of current and trashed diagrams plus retained versions for the current guest session. It can also irreversibly delete all diagrams owned by that session after typing the exact confirmation <code>DELETE MY DATA</code>. The export deliberately excludes internal owner hashes and revoked historical share capabilities. Because ownership is cookie-bound, another browser or a lost cookie cannot prove control of the same guest records through this self-service endpoint.</p>
    <p>Depending on applicable law, you may also request access, rectification, erasure, restriction, objection, or portability and lodge a complaint with a competent supervisory authority. Identity may need to be verified before records outside the current guest session can be discussed. The <a href="https://eur-lex.europa.eu/eli/reg/2016/679/oj">official GDPR text</a> describes these rights and their conditions.</p>
    <h2>Security and changes</h2>
    <p>State-changing requests use CSRF protection, session ownership checks, rate limits, and security logging. No internet service can guarantee absolute security. Material changes should update the date and this notice before or when the changed processing begins.</p>
  </main>
</body>
</html>)HTML";
}

} // namespace nuigraph
