# NuiGraph Studio

NuiGraph Studio is a production-oriented C++ graph and diagram editor deployed at
`https://nuicpp.micutu.com`. It is built for flowcharts, technical diagrams,
architecture maps, service graphs, and node-link diagrams.

Anyone can use it without an account: visitors get a signed guest session, own
whatever they create in it, and can share read-only links. There is a single
admin account for maintenance.

- **Security model:** [`docs/SECURITY.md`](docs/SECURITY.md)
- **Deploy runbook:** [`docs/DEPLOY.md`](docs/DEPLOY.md)

## Features

### Editing

- SVG canvas with grid, pan, zoom, selection, node dragging, and immediate
  updates in the browser.
- Node types: `process`, `decision`, `database`, `service`, `api`, `note`,
  `external`. Directed and undirected labelled edges with colours and arrowheads.
- Snap-to-grid, simple auto-layout, minimap, copy/paste, undo/redo.
- Multi-select by Shift-click, by rubber-band drag on empty canvas (Shift-drag
  extends), or by long-press on touch.
- Keyboard shortcuts with a `?` overlay listing them.
- Diagram search filter over the sidebar list.
- Light and dark themes, remembered per browser.

### Diagrams

- Create, list, open, rename, duplicate, delete, save, import, export.
- Version history with automatic snapshots on save, manual snapshots, and
  restore. Retention is bounded by `MAX_VERSIONS_PER_DIAGRAM`.
- Export to JSON, SVG, PNG, and Mermaid.
- Template gallery: architecture, incident-response, and data-pipeline diagrams.
- Public share links at `/d/{slug}` with read-only viewing and one-click fork.
- Browser draft autosave with a restore prompt for unsaved local changes.

### Mobile and offline

- Full pointer-event input: touch drag, pinch-zoom, long-press for additive
  selection. The canvas sets `touch-action: none` so gestures do not scroll the
  page.
- Three-breakpoint responsive layout — four-column desktop, stacked tablet, and
  a phone layout with a full-bleed canvas and slide-up sheets for the panels.
- Installable as a PWA (`manifest.webmanifest`, maskable icon).
- Service worker caches the static shell for offline loads and serves
  `/offline.html` when a navigation cannot reach the network. It never caches
  `/api/`, `/login`, `/logout`, or `/health` — see the caching section of
  [`docs/SECURITY.md`](docs/SECURITY.md) for why that matters.

### Operations

- Bounded PostgreSQL connection pool with RAII leases and liveness checks.
- Graceful shutdown on SIGTERM/SIGINT: the listener stops and in-flight requests
  drain before exit, so a restart cannot truncate a save.
- Structured access logging with ISO 8601 timestamps.
- Static assets served with `ETag` and `Cache-Control: no-cache`, so deploys
  reach visitors on their next request at the cost of a `304`.
- Public `/health` endpoint returning only service status.

### Security

Summarised here, detailed in [`docs/SECURITY.md`](docs/SECURITY.md).

- Cloudflare-only ingress enforced at nginx; direct-to-origin requests get `403`.
- HMAC-signed, `HttpOnly`, `SameSite=Lax` session cookies; 8-hour admin sessions,
  7-day guest sessions.
- PBKDF2-SHA256 admin password with constant-time comparison.
- Per-session ownership by HMAC-derived owner hash; read and edit both checked
  by diagram id on every path that emits diagram content, including duplicate.
- CSRF tokens bound to the exact session cookie, required on every mutating
  endpoint.
- Rate limiting at the edge and in the app, plus a per-guest diagram quota so
  storage growth has a ceiling and not just a slope.
- Defensive id parsing, exhaustive input validation, parameterised SQL only,
  no shell execution, no user-controlled filesystem paths.
- CSP without `unsafe-inline` for scripts, plus the usual header set, sent by
  both nginx and the app.

## Stack

- C++20, CMake
- Nui C++ frontend compiled with Emscripten to WebAssembly
- cpp-httplib for HTTP serving
- libpqxx/libpq for PostgreSQL
- nlohmann/json
- OpenSSL for PBKDF2, HMAC-SHA256, SHA-256, and CSPRNG
- PostgreSQL 18
- nginx reverse proxy behind Cloudflare, Certbot TLS

## How the frontend fits together

The authenticated editor shell is rendered by a Nui C++ frontend compiled to
`public/nui/index.js` plus `index.wasm`. The backend serves that shell for `/`,
so the top bar, panels, toolbar, and SVG canvas root are created from C++ in the
browser. `public/editor.js` is a JavaScript bridge that attaches canvas
interaction handlers and calls the JSON API.

If the WebAssembly module is missing, `nuiShellReady()` detects the incomplete
bundle and falls back to the server-rendered editor rather than serving a shell
that can never start.

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

Five suites: unit tests for validation and rate-limiter behaviour, a frontend
asset check, a live security smoke test, an API integration test, and a rate
limit test. The three live suites start a throwaway instance on a free port,
skip cleanly if the database is unavailable, and delete their own rows on exit.

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
AUTH_USERNAME  AUTH_PASSWORD_HASH  SESSION_SECRET  PUBLIC_ACCESS
MAX_NODES_PER_DIAGRAM  MAX_EDGES_PER_DIAGRAM  MAX_VERSIONS_PER_DIAGRAM
MAX_IMPORT_BYTES  MAX_DIAGRAMS_PER_GUEST  DB_POOL_SIZE  PROJECT_ROOT
```

Never commit `.env`, `.initial_admin_password`, or generated secrets.

## Database

Database `nuigraph_studio`, user `nuigraph_user`. Schema lives in `migrations/`
and is applied by `scripts/migrate.sh`. Runtime grants are least-privilege:

```bash
psql -d nuigraph_studio -f deploy/postgres/runtime_privileges.sql
```

## Deployment

Full runbook in [`docs/DEPLOY.md`](docs/DEPLOY.md). The short version:

```bash
cmake --build build --config Release -j "$(nproc)"
ctest --test-dir build --output-on-failure
scripts/migrate.sh
sudo systemctl restart nuigraph-studio.service
scripts/deploy_check.sh
```

nginx config is versioned in `deploy/nginx/`. Both files are needed: the vhost
and the `conf.d/` rate-limit zones it references. Always
`sudo nginx -t && sudo systemctl reload nginx` — this host serves many other
sites, so never restart nginx.

## API

| Method | Path |
| --- | --- |
| `GET` | `/health` |
| `GET` | `/` |
| `GET` | `/docs` |
| `GET` | `/d/{slug}` |
| `GET` | `/api/session` |
| `GET` | `/api/diagrams` |
| `POST` | `/api/diagrams` |
| `GET` | `/api/diagrams/{id}` |
| `PUT` | `/api/diagrams/{id}` |
| `DELETE` | `/api/diagrams/{id}` |
| `GET` | `/api/diagrams/slug/{slug}` |
| `POST` | `/api/diagrams/{id}/duplicate` |
| `GET` | `/api/diagrams/{id}/versions` |
| `POST` | `/api/diagrams/{id}/versions` |
| `POST` | `/api/diagrams/{id}/restore/{versionId}` |
| `GET` | `/api/diagrams/{id}/export.json` |
| `POST` | `/api/diagrams/import` |
| `GET` | `/api/templates` |
| `POST` | `/api/templates/{key}/create` |
| `GET` | `/manifest.webmanifest`, `/sw.js`, `/offline.html`, `/icons/{name}` |

Mutating endpoints require `X-CSRF-Token` from `/api/session`. Template keys:
`cloud-architecture`, `incident-response`, `data-pipeline`.

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
- `style-src 'unsafe-inline'` is still required by the editor's inline style
  attributes.
- Rate limits are per process; running a second instance would double them.
- The admin diagram list is unpaginated, which will not scale indefinitely.
- Remove `.initial_admin_password` once the password is in a password manager.
