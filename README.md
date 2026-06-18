# NuiGraph Studio

NuiGraph Studio is a production-oriented C++ graph and diagram editor deployed at `https://nuicpp.micutu.com`. It is built for flowcharts, technical diagrams, architecture maps, service graphs, and node-link diagrams.

## Features

- Dark visual SVG editor with grid, pan, zoom, selectable objects, node dragging, and immediate browser updates.
- Diagram CRUD: create, list, open, rename, duplicate, delete, save, import, and JSON export.
- Sample architecture diagram loader for demos.
- Public share links with read-only viewing and one-click fork/duplicate behavior.
- Template gallery with ready-to-use architecture, incident-response, and data-pipeline diagrams.
- Browser draft autosave with restore prompt for unsaved local changes.
- SVG and PNG export from the visual canvas.
- Snap-to-grid toggle, simple auto-layout, minimap, copy/paste, and Shift-click multi-select basics.
- Node types: `process`, `decision`, `database`, `service`, `api`, `note`, `external`.
- Directed and undirected labeled edges with colors and arrowheads.
- Basic undo/redo in the browser for common editing operations.
- PostgreSQL persistence for diagrams, nodes, edges, and bounded version snapshots.
- Version history with manual snapshots and restore.
- Admin login with PBKDF2-SHA256 password hash in `.env`.
- Public guest sessions when `PUBLIC_ACCESS=true`; owners and admins can edit, other visitors can view and fork.
- CSRF protection for mutating JSON endpoints and login rate limiting.
- In-process API write/create rate limiting for public-access abuse control.
- Public `/health` endpoint with non-sensitive JSON.

## Stack

- C++20
- CMake
- Nui C++ frontend compiled with Emscripten/WASM
- cpp-httplib for HTTP serving
- libpqxx/libpq for PostgreSQL
- nlohmann/json for JSON parsing/serialization
- OpenSSL crypto for password verification and signed sessions
- PostgreSQL database `nuigraph_studio`
- Nginx reverse proxy and Certbot TLS

## Nui Usage Notes

The authenticated editor shell is rendered by a Nui C++ frontend compiled with Emscripten to `/public/nui/index.js`. The backend serves `/public/nui/index.html` for `/` after login, so the top bar, panels, toolbar, and SVG canvas root are created from C++/Nui in the browser. A small JavaScript bridge in `/public/editor.js` attaches canvas interaction handlers and calls the C++ backend JSON API.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
scripts/build_nui_frontend.sh
```

## Run On The VPS

```bash
./build/nuigraph-studio /home/micu/nuicpp/.env
```

The application binds only to `127.0.0.1:${APP_PORT}`. The selected production port is stored in `.env`.

## Environment

`.env` is required and ignored by Git. It contains:

- `APP_ENV`
- `APP_HOST`
- `APP_PORT`
- `APP_BASE_URL`
- `DATABASE_URL`
- `AUTH_USERNAME`
- `AUTH_PASSWORD_HASH`
- `PUBLIC_ACCESS`
- `SESSION_SECRET`
- `MAX_NODES_PER_DIAGRAM`
- `MAX_EDGES_PER_DIAGRAM`
- `MAX_VERSIONS_PER_DIAGRAM`
- `MAX_IMPORT_BYTES`
- `PROJECT_ROOT`

Do not commit `.env`, `.initial_admin_password`, or generated secrets.

## PostgreSQL

Preferred database and user:

- Database: `nuigraph_studio`
- User: `nuigraph_user`

Run migrations with:

```bash
scripts/migrate.sh
```

Schema files live in `migrations/`.

After migrations, apply least-privilege runtime grants from a database owner
or PostgreSQL superuser:

```bash
psql -d nuigraph_studio -f deploy/postgres/runtime_privileges.sql
```

## Systemd

Service name:

```text
nuigraph-studio.service
```

Example unit:

```text
systemd/nuigraph-studio.service.example
```

Installed unit path:

```text
/etc/systemd/system/nuigraph-studio.service
```

Useful commands:

```bash
sudo systemctl status nuigraph-studio.service --no-pager
sudo journalctl -u nuigraph-studio.service -n 100 --no-pager
sudo systemctl restart nuigraph-studio.service
```

## Nginx

Nginx reverse proxy config path:

```text
/etc/nginx/sites-available/nuicpp.micutu.com
```

Versioned hardened template:

```text
deploy/nginx/nuicpp.micutu.com.conf
```

The enabled symlink is expected at:

```text
/etc/nginx/sites-enabled/nuicpp.micutu.com
```

Always validate before reload:

```bash
sudo nginx -t
sudo systemctl reload nginx
```

## Public URL

```text
https://nuicpp.micutu.com
```

## API Endpoints

- `GET /health`
- `GET /`
- `GET /docs`
- `GET /api/diagrams`
- `GET /api/templates`
- `POST /api/templates/{key}/create`
- `POST /api/diagrams`
- `GET /api/diagrams/{id}`
- `GET /api/diagrams/slug/{slug}`
- `PUT /api/diagrams/{id}`
- `DELETE /api/diagrams/{id}`
- `POST /api/diagrams/{id}/duplicate`
- `GET /api/diagrams/{id}/versions`
- `POST /api/diagrams/{id}/versions`
- `POST /api/diagrams/{id}/restore/{versionId}`
- `GET /api/diagrams/{id}/export.json`
- `POST /api/diagrams/import`

When `PUBLIC_ACCESS=true`, visitors receive a signed guest session from `/api/session`. Diagram owners and admins can edit; non-owners can open share links read-only and duplicate/fork diagrams into their own session. `/health` is public.

Built-in template keys include `cloud-architecture`, `incident-response`, and `data-pipeline`.

## Diagram JSON Format

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

## Versioning

Each save creates a version snapshot in `diagram_versions`. Manual snapshots can also be created from the UI. Version retention is bounded by `MAX_VERSIONS_PER_DIAGRAM`; older snapshots are pruned to avoid unbounded database growth.

## Security Notes

- The app binds to `127.0.0.1` only.
- Nginx terminates TLS and proxies to the local app port.
- When `PUBLIC_ACCESS=true`, visitors can use the editor without an admin login. Mutating endpoints still require a signed anonymous session and CSRF token from `/api/session`.
- New diagrams are tied to the creating guest session by a hashed owner token. Share URLs use `/d/{slug}`; other visitors get a read-only view and can duplicate the diagram to edit their own copy.
- Admin login remains available at `/login`.
- Passwords are verified using PBKDF2-SHA256 hashes.
- Mutating API endpoints require `X-CSRF-Token` from `/api/session`.
- Login attempts are rate-limited in the C++ app.
- Mutating API requests are rate-limited by client IP, with stricter limits for create/import/fork/template creation actions.
- Nginx sends HSTS, CSP, frame, content-type, referrer, and permissions policy headers.
- Input validation limits title lengths, description lengths, node count, edge count, import size, colors, keys, coordinates, and node sizes.
- Import rejects invalid JSON and edges pointing to missing nodes.
- The web app does not execute shell commands and does not expose filesystem path access.
- `/health` returns only service status.

## Deployment Notes

Deployment uses:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
scripts/build_nui_frontend.sh
scripts/migrate.sh
sudo systemctl enable --now nuigraph-studio.service
sudo nginx -t
sudo systemctl reload nginx
sudo certbot --nginx -d nuicpp.micutu.com --email alex_mihai984@yahoo.com --agree-tos --no-eff-email
```

Git commits and pushes are manual and were not done by the agent.

## Troubleshooting

```bash
scripts/deploy_check.sh
ctest --test-dir build --output-on-failure
curl -fsS http://127.0.0.1:${APP_PORT}/health
sudo journalctl -u nuigraph-studio.service -n 100 --no-pager
sudo nginx -t
sudo systemctl status nginx --no-pager
psql "$DATABASE_URL" -c "\\dt"
```

## Limitations And TODOs

- The editor shell/canvas root and toolbar are Nui C++/WASM; detailed canvas gestures still use a small JavaScript bridge.
- The initial admin password file should be removed after the password is stored in a password manager and/or rotated.
- Collaboration/WebSocket editing is not implemented in v1.
- Guest ownership is browser/session based, not full multi-account identity. Clearing cookies can lose owner edit access unless the admin account is used.
- Undo/redo is browser-local and basic; persisted granular command history is a future improvement.
- Draft autosave is browser-local and is not a replacement for the PostgreSQL Save action/version snapshot.
- Mobile and tablet layouts are functional but the polished target is desktop.
