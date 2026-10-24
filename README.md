# NuiGraph Studio

NuiGraph Studio is a production-oriented C++ graph and diagram editor deployed at
`https://nuicpp.micutu.com`. It is built for flowcharts, technical diagrams,
architecture maps, service graphs, and node-link diagrams.

Anyone can use it without an account: visitors get a signed guest session, own
whatever they create in it, and can explicitly enable read-only links. There is a single
admin account for maintenance.

- **Security model:** [`docs/SECURITY.md`](docs/SECURITY.md)
- **Security audit and threat model:** [`docs/SECURITY_AUDIT.md`](docs/SECURITY_AUDIT.md)
- **Incident response:** [`docs/INCIDENT_RESPONSE.md`](docs/INCIDENT_RESPONSE.md)
- **Monitoring and alert response:** [`docs/MONITORING.md`](docs/MONITORING.md)
- **Supply-chain inventory:** [`docs/SUPPLY_CHAIN.md`](docs/SUPPLY_CHAIN.md)
- **Delivery roadmap:** [`docs/ROADMAP.md`](docs/ROADMAP.md)
- **Deploy runbook:** [`docs/DEPLOY.md`](docs/DEPLOY.md)

## Features

### Editing

- SVG canvas with grid, pan, zoom, selection, node dragging, and immediate
  updates in the browser.
- Node types: `process`, `decision`, `database`, `service`, `api`, `note`,
  `external`. Directed and undirected labelled edges with colours and arrowheads.
- Configurable 8/16/32/64 px visual grid and matching snap spacing, remembered
  locally; simple auto-layout, minimap, copy/paste and undo/redo. Multi-node
  selections can align to six axes or distribute equal centre spacing.
- Multi-select by Shift-click, by rubber-band drag on empty canvas (Shift-drag
  extends), or by long-press on touch.
- Multi-selection inspector summarises type, colour and selection bounds, then
  applies type, colour, width and height changes to every selected node as one
  undoable edit.
- Keyboard movement for one or many selected nodes uses the active grid step;
  `Shift+Arrow` moves five steps and held-key repeats collapse into one undo.
- `Ctrl/Cmd+A` selects every node for immediate framing, alignment, batch
  inspection or movement, and is also available from commands/mobile actions.
- Select Connected Component expands one or more selected nodes across weak
  graph connectivity in linear time, while excluding disconnected subgraphs;
  it is available from the command palette and customisable mobile actions.
- Select Direct Neighbors expands a selection by exactly one incoming or
  outgoing relationship, making local dependency inspection fast without
  losing the original seed nodes.
- Source/Sink selection finds directional entry points and outputs in one
  action; undirected relationships count both ways, invalid endpoints are
  ignored and isolated nodes correctly qualify as both.
- `Ctrl/Cmd+D` duplicates selected nodes plus their internal edges as one
  undoable edit; collision-safe keys remain valid even when originals use the
  full 64-character server limit.
- `Ctrl/Cmd+X` cuts selected nodes and their internal relationships into the
  in-memory clipboard as one undoable edit; paste regenerates collision-safe
  keys and remains blocked for read-only shares.
- Persistent geometry locks protect selected nodes from drag, resize, keyboard
  movement, batch sizing, alignment and auto-layout while keeping selection,
  labels and visual properties editable; lock state has an SVG/forced-colour cue.
- Eight resize handles on a single selected node, honouring the same 72x48 to
  600x400 bounds the server clamps to.
- Alignment guides while dragging: edges and centres latch onto neighbouring
  nodes within 6 screen pixels, and the guide spans every box it lines up.
- Searchable command palette on `Ctrl/Cmd+K`, including editing, canvas, export,
  panel and appearance actions; keyboard shortcuts remain available from `?`.
- Instant node navigator on `/` searches titles, types and stable keys, then
  selects and centres a result without leaving the canvas or using the network.
- Accessible diagram outline on `Alt+O` exposes nodes and edges as a searchable
  ARIA tree with keyboard navigation, selection and canvas centring.
- Polite screen-reader status announcements describe canvas selection changes
  and draft persistence without adding visual noise or changing layout.
- Modal dialogs and mobile property/diagram sheets contain keyboard focus,
  close on Escape and restore focus to the control that opened them.
- Automated axe-core scans gate serious and critical accessibility regressions
  across desktop, command-dialog, phone and mobile-sheet browser states.
- Public JavaScript is gated against `innerHTML`, adjacent-markup insertion,
  dynamic code execution and document writes; API/DB text is rendered through
  DOM text nodes and covered with malicious-title browser tests.
- Public editor SEO includes canonical and language alternates, Open Graph and
  Twitter metadata, SoftwareApplication microdata, a semantic `h1`, sitemap,
  robots policy and an installable categorised PWA manifest.
- Inline node and edge-label editing by double-click, `F2`, `Enter`, command
  palette or a pinned mobile action, with one-step undo and browser-draft
  persistence.
- Browser-local Diagram Review detects empty diagrams, missing titles or labels,
  orphan nodes, invalid or duplicate edges, self-loops and directed cycles, then
  focuses the selected finding without sending diagram content to another service.
- Zoom-to-fit frames the entire graph while reserving room for mobile controls.
- Actual Size resets zoom to 100% around the viewport centre from the clickable
  zoom indicator, `Shift+1`, the command palette or a pinned mobile action.
- Frame Selection (`Shift+2`, command palette or mobile action) centres and
  scales a selected node set or both endpoints of a selected edge.
- Diagram search filter over the sidebar list.
- Light and dark themes, remembered per browser.
- System forced-colour/high-contrast support preserves node, edge, selection,
  resize and focus states; reduced-motion preferences disable transitions and
  animations without removing functionality.

### Diagrams

- Create, list, open, rename, duplicate, delete, save, import, export.
- Version history with automatic snapshots on save, manual snapshots, restore,
  and a diff view listing what changed between a snapshot and the open diagram.
  Retention is bounded by `MAX_VERSIONS_PER_DIAGRAM`.
- Optimistic revisions with strong ETags: stale saves/restores are rejected and
  the browser keeps the losing tab's draft for recovery.
- Export to JSON, SVG, PNG, and Mermaid.
- Template gallery: architecture, incident-response, and data-pipeline diagrams.
- Private-by-default diagrams with explicit read-only sharing at `/d/{slug}`,
  128-bit capabilities, allowlisted 5-minute to 30-day expiry, immediate
  rotation/revocation and one-click fork. The UI defaults new links to 7 days.
- Browser draft autosave with a restore prompt for unsaved local changes.

### Mobile and offline

- Full pointer-event input: touch drag, pinch-zoom, long-press for additive
  selection. The canvas sets `touch-action: none` so gestures do not scroll the
  page.
- Three-breakpoint responsive layout — four-column desktop, stacked tablet, and
  a phone layout with a full-bleed canvas and slide-up sheets for the panels.
- Compact phone action bar replaces both scrolling toolbars. Its two quick
  actions are customisable and persisted locally, while every action remains in
  the command palette; coarse-pointer targets are at least 44px.
- Installable as a PWA (`manifest.webmanifest`, maskable icon).
- Service worker caches the static shell for offline loads and serves
  `/offline.html` when a navigation cannot reach the network. It never caches
  `/api/`, authentication, liveness or readiness responses — see the caching
  section of [`docs/SECURITY.md`](docs/SECURITY.md) for why that matters.

### Operations

- Bounded PostgreSQL connection pool with RAII leases and liveness checks.
- Graceful shutdown on SIGTERM/SIGINT: the listener stops and in-flight requests
  drain before exit, so a restart cannot truncate a save.
- Structured access logging with ISO 8601 timestamps.
- Static asset URLs and the service-worker cache name use a content-derived
  release digest; matching assets are immutable, while the worker is never
  stored and its registration bypasses the HTTP cache for updates.
- Search metadata, canonical URLs, `robots.txt`, and `sitemap.xml` cover the
  public editor and documentation; bearer share links, auth, API, and probes
  are explicitly excluded from indexing.
- Separate `/live` process and PostgreSQL-backed `/ready` probes; `/health`
  remains a compatibility alias for liveness. Dependency errors are not exposed.
- Loopback-only Prometheus telemetry for bounded HTTP/security/create, latency,
  row/logical/database-capacity and connection-pool signals, with validated
  alert rules and an operator response runbook in
  [`docs/MONITORING.md`](docs/MONITORING.md).

### Security

Summarised here, detailed in [`docs/SECURITY.md`](docs/SECURITY.md).

- Cloudflare-only ingress enforced at nginx; direct-to-origin requests get `403`.
- Purpose-separated HMAC keys for `HttpOnly`, `SameSite=Strict` sessions, CSRF,
  stable guest ownership and rate-limit pseudonyms; HTTPS sessions use a
  browser-enforced `__Host-` cookie, while signing-key and legacy-name rollover
  transparently preserve active 8-hour admin and 7-day guest sessions.
- PBKDF2-SHA256 admin password with constant-time comparison.
- Per-session ownership by HMAC-derived owner hash; read and edit both checked
  by diagram id on every path that emits diagram content, including duplicate.
- CSRF tokens bound to the exact session cookie on authenticated JSON writes,
  backed by a central Fetch Metadata firewall that rejects browser mutations
  unless `Sec-Fetch-Site` identifies the exact application origin.
- Present `Origin` headers on state-changing requests must canonically match
  `APP_BASE_URL`; opaque, foreign and duplicate origins fail before routing,
  while non-browser clients may omit the header.
- Strict request media types: API bodies accept only `application/json`, login
  accepts only form encoding, and the frontend does not attach a JSON type to
  bodyless reads or deletes.
- Rate limiting at the edge and in the app, plus atomic per-guest and global
  diagram and logical-byte quotas so concurrent writes cannot grow retained
  user content past the configured ceilings.
- End-to-end request IDs and redacted structured security events for login,
  authorization, CSRF, cross-site/origin/media-type rejection, quota,
  rate-limit, share lifecycle/view and storage failures.
- Defensive id parsing, exhaustive input validation, parameterised SQL only,
  no shell execution, no user-controlled filesystem paths.
- CSP without `unsafe-inline` for scripts or styles, plus the usual header set,
  explicit frame/media/worker/manifest restrictions, origin-agent isolation and
  feature permissions, sent consistently by both nginx and the app.
- User/session shells, bearer shares, login/logout, APIs and probes are guarded
  centrally with browser, CDN and Cloudflare `no-store` policies plus
  `Vary: Cookie`; immutable caching remains limited to digest-matched assets.

## Stack

- C++20, CMake
- Server-rendered C++ shell, with an optional Nui/Emscripten WebAssembly shell
- cpp-httplib for HTTP serving
- libpqxx 7.10+ / libpq for PostgreSQL
- nlohmann/json
- OpenSSL for PBKDF2, HMAC-SHA256, SHA-256, and CSPRNG
- PostgreSQL 18
- nginx reverse proxy behind Cloudflare, Certbot TLS

## How the frontend fits together

Production deliberately defaults to `FRONTEND_SHELL=server`: the backend renders
the top bar, panels, toolbar and SVG canvas root, while `public/editor.js`
attaches interactions and calls the JSON API. This is the complete, tested
primary path and does not need WebAssembly code generation in its CSP.

`FRONTEND_SHELL=nui` is an explicit alternative. It renders the same shell from
a Nui C++ frontend compiled to `public/nui/index.js` plus a `.wasm` module.
Configuration validation refuses to start or release this mode if its HTML,
loader or regular (non-symlink) WebAssembly artifact is missing; there is no
silent fallback that can hide a broken build.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
scripts/build_nui_frontend.sh    # only when nui-frontend/ changed; needs emsdk
```

The build is clean under `-Wall -Wextra -Wpedantic`.

## Test

```bash
ctest --test-dir build --output-on-failure
```

Fifteen suites cover unit validation, frontend assets, the nginx candidate,
Prometheus rules, incident-runbook integrity, secret scanning, CycloneDX SBOM generation, real
Chromium behavior, live security/API behavior, shared persistent rate limits,
concurrent row/logical-byte quota enforcement, signing-key rotation, and
migration-ledger integrity. The eight database-backed suites use
a config with `APP_ENV=test` whose
actual PostgreSQL database and login-role names both end in `_test`. Missing
local prerequisites appear as **Not Run**, not as false passes; deployment sets
`NUIGRAPH_REQUIRE_LIVE_TESTS=1`, so there they fail closed instead of skipping.
See the deploy runbook for the isolated test-database setup.

The Chromium suite exercises desktop and phone layouts, CSP enforcement,
service-worker replacement, cache isolation from authenticated APIs, and a real
offline navigation. It also verifies the local review and arrangement engines,
node navigator, safe inline label editing, undoable multi-node alignment, command-palette focus restoration,
mobile action customisation, 44px touch targets and
persistence across reload. CI
installs its pinned browser runtime; locally run
`nui-frontend/node_modules/.bin/playwright install chromium` after `npm ci`.

`scripts/scan_secrets.py` rejects provider tokens, private-key markers,
credential-bearing database URLs and generic high-entropy credential
assignments without echoing candidate values. It scans tracked and untracked
non-ignored source files locally and runs as a dedicated CTest/CI gate.

GitHub Actions runs the full build and all live suites against a disposable
PostgreSQL database, plus ShellCheck, Cppcheck, npm audit, and CodeQL. Workflow
actions and the PostgreSQL service image are pinned to immutable revisions;
Dependabot proposes weekly action and frontend dependency updates. Successful CI
also publishes native/runtime and full npm CycloneDX inventories for the exact
commit; see [`docs/SUPPLY_CHAIN.md`](docs/SUPPLY_CHAIN.md).

## Run

```bash
./build/nuigraph-studio /home/micu/nuicpp/.env
```

The application binds only to `127.0.0.1:${APP_PORT}`. In production it runs
under systemd as `nuigraph-studio.service`.

## Environment

`.env` is required, mode `600`, and git-ignored. Variables and their production
values are documented in [`docs/DEPLOY.md`](docs/DEPLOY.md#2-environment):

```
APP_ENV  APP_HOST  APP_PORT  APP_BASE_URL  DATABASE_URL
AUTH_USERNAME  AUTH_PASSWORD_HASH  SESSION_SIGNING_SECRET
SESSION_SIGNING_SECRET_PREVIOUS  CSRF_SECRET  OWNER_HASH_SECRET
RATE_LIMIT_SECRET  PUBLIC_ACCESS  FRONTEND_SHELL
MAX_NODES_PER_DIAGRAM  MAX_EDGES_PER_DIAGRAM  MAX_VERSIONS_PER_DIAGRAM
MAX_IMPORT_BYTES  MAX_DIAGRAMS_PER_GUEST  MAX_DIAGRAMS_TOTAL
MAX_LOGICAL_BYTES_PER_GUEST  MAX_LOGICAL_BYTES_TOTAL  DB_POOL_SIZE
PROJECT_ROOT
```

Never commit `.env`, `.initial_admin_password`, or generated secrets.

## Database

Database `nuigraph_studio`, user `nuigraph_user`. Schema lives in `migrations/`
and is applied transactionally by `scripts/migrate.sh`. Applied filenames,
SHA-256 checksums and timestamps live in the owner-only
`nuigraph_meta.schema_migrations` ledger. Never edit an applied migration; add a
new numbered file. Runtime grants are least-privilege:

```bash
psql -d nuigraph_studio -f deploy/postgres/runtime_privileges.sql
```

## Deployment

Full runbook in [`docs/DEPLOY.md`](docs/DEPLOY.md). The short version:

```bash
./scripts/deploy.sh
```

Production runs a root-owned atomic release as the dedicated `nuigraph` user.
Its runtime PostgreSQL role is distinct from the database-owning migration role,
and its config is delivered through a private systemd credential mount. The
deploy script is fail-closed and restores the previous release on failed health.

nginx config is versioned in `deploy/nginx/`. Both files are needed: the vhost
and the `conf.d/` rate-limit zones it references. Always
`sudo nginx -t && sudo systemctl reload nginx` — this host serves many other
sites, so never restart nginx.

## API

| Method | Path |
| --- | --- |
| `GET` | `/health` |
| `GET` | `/live` |
| `GET` | `/ready` |
| `GET` | `/` |
| `GET` | `/docs` |
| `GET` | `/robots.txt`, `/sitemap.xml` |
| `GET` | `/d/{slug}` |
| `GET` | `/api/session` |
| `GET` | `/api/diagrams` |
| `POST` | `/api/diagrams` |
| `GET` | `/api/diagrams/{id}` |
| `PUT` | `/api/diagrams/{id}` |
| `DELETE` | `/api/diagrams/{id}` |
| `GET` | `/api/diagrams/slug/{slug}` |
| `POST` | `/api/diagrams/slug/{slug}/fork` |
| `POST` | `/api/diagrams/{id}/sharing` |
| `POST` | `/api/diagrams/{id}/duplicate` |
| `GET` | `/api/diagrams/{id}/versions` |
| `POST` | `/api/diagrams/{id}/versions` |
| `GET` | `/api/diagrams/{id}/versions/{versionId}` |
| `POST` | `/api/diagrams/{id}/restore/{versionId}` |
| `GET` | `/api/diagrams/{id}/export.json` |
| `POST` | `/api/diagrams/import` |
| `GET` | `/api/templates` |
| `POST` | `/api/templates/{key}/create` |
| `GET` | `/manifest.webmanifest`, `/sw.js`, `/offline.html`, `/offline.js`, `/icons/{name}` |

Mutating endpoints require `X-CSRF-Token` from `/api/session`. Template keys:
`cloud-architecture`, `incident-response`, `data-pipeline`.

`PUT /api/diagrams/{id}`, `DELETE /api/diagrams/{id}`, sharing changes and
version restore also require the exact `ETag` from the latest diagram response in `If-Match`. Missing
preconditions return `428`; stale revisions return `412` with
`current_revision` and do not mutate storage. The JSON response includes the
monotonically increasing `revision` value.

`POST /api/diagrams/{id}/sharing` accepts an `action` of `enable`, `rotate` or
`disable`. Enable and rotate always mint a new capability; disable makes the
current `/d/{slug}` and slug API paths return `404` immediately.

`GET /api/diagrams` is paged: `?limit=` (default 100, max 200) and `?offset=`
(max 100000). Out-of-range or unparseable values are clamped rather than
rejected. The response carries `total`, `limit`, `offset`, and `has_more`
alongside `diagrams`. A page exceeds `MAX_DIAGRAMS_PER_GUEST`, so only the admin
view ever pages.

## Diagram JSON format

```json
{
  "title": "Example diagram",
  "description": "",
  "nodes": [
    {
      "key": "node_1",
      "type": "process",
      "title": "Build",
      "x": 120,
      "y": 80,
      "width": 160,
      "height": 80,
      "color": "#38bdf8",
      "metadata": {}
    }
  ],
  "edges": [
    {
      "key": "edge_1",
      "source": "node_1",
      "target": "node_2",
      "label": "calls",
      "directed": true,
      "color": "#94a3b8",
      "metadata": {}
    }
  ]
}
```

## Known limitations

- Guest ownership is browser-bound. Clearing cookies or waiting out the 7-day
  session loses edit access, and there is no recovery path — there is no
  identity to prove. Use the admin account for anything that must persist.
- No collaboration or WebSocket editing.
- Undo/redo is browser-local and coarse; there is no persisted command history.
- Draft autosave is browser-local and is not a substitute for Save.
- Revision conflicts preserve the local draft but do not merge it automatically;
  reopen the diagram and deliberately restore/reapply the draft.
- Application rate limits are shared in PostgreSQL and fail closed for login and
  writes if their decision cannot be persisted; Cloudflare/nginx still provide
  the cheaper edge layer.
- Never retain `.initial_admin_password` on the production checkout; move any
  recovery copy to root-only incident storage after the password is recorded.
