# NuiGraph Studio

NuiGraph Studio is a production-oriented C++ graph and diagram editor deployed at `https://nuicpp.micutu.com`. It is built for flowcharts, technical diagrams, architecture maps, service graphs, and node-link diagrams.

## Features

- Dark visual SVG editor with grid, pan, zoom, selectable objects, node dragging, and immediate browser updates.
- Diagram CRUD: create, list, open, rename, duplicate, delete, save, import, and JSON export.
- Node types: `process`, `decision`, `database`, `service`, `api`, `note`, `external`.
- Directed and undirected labeled edges with colors and arrowheads.
- Basic undo/redo in the browser for common editing operations.
- PostgreSQL persistence for diagrams, nodes, edges, and bounded version snapshots.
- Version history with manual snapshots and restore.
- Admin login with PBKDF2-SHA256 password hash in `.env`.
- Public `/health` endpoint with non-sensitive JSON.

## Stack

- C++20
- CMake
- cpp-httplib for HTTP serving
- libpqxx/libpq for PostgreSQL
- nlohmann/json for JSON parsing/serialization
- OpenSSL crypto for password verification and signed sessions
- PostgreSQL database `nuigraph_studio`
- Nginx reverse proxy and Certbot TLS

## Nui Usage Notes

Nui C++ is a WebView/WASM-oriented UI library and is useful for C++ UI structure in desktop/WebView builds. For this VPS public browser deployment, the production build uses a C++ HTTP backend plus a browser SVG frontend. C++ remains the primary implementation for model validation, persistence, routing, authentication, and API behavior. Direct Nui WebView runtime was not used because it is not a server deployment target by itself.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
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
- `POST /api/diagrams`
- `GET /api/diagrams/{id}`
- `PUT /api/diagrams/{id}`
- `DELETE /api/diagrams/{id}`
- `POST /api/diagrams/{id}/duplicate`
- `GET /api/diagrams/{id}/versions`
- `POST /api/diagrams/{id}/versions`
- `POST /api/diagrams/{id}/restore/{versionId}`
- `GET /api/diagrams/{id}/export.json`
- `POST /api/diagrams/import`

All editing endpoints require admin authentication. `/health` is public.

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
- Editing endpoints require signed-session admin login.
- Passwords are verified using PBKDF2-SHA256 hashes.
- Input validation limits title lengths, description lengths, node count, edge count, import size, colors, keys, coordinates, and node sizes.
- Import rejects invalid JSON and edges pointing to missing nodes.
- The web app does not execute shell commands and does not expose filesystem path access.
- `/health` returns only service status.

## Deployment Notes

Deployment uses:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
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
curl -fsS http://127.0.0.1:${APP_PORT}/health
sudo journalctl -u nuigraph-studio.service -n 100 --no-pager
sudo nginx -t
sudo systemctl status nginx --no-pager
psql "$DATABASE_URL" -c "\\dt"
```

## Limitations And TODOs

- Direct Nui WebView runtime is not used in this public server deployment; the frontend is browser SVG served by the C++ app.
- Collaboration/WebSocket editing is not implemented in v1.
- Undo/redo is browser-local and basic; persisted granular command history is a future improvement.
- Mobile and tablet layouts are functional but the polished target is desktop.
