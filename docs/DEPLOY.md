# Deploy runbook

Production host: the VPS. Public URL: `https://nuicpp.micutu.com`.
Everything below assumes `PROJECT_ROOT=/home/micu/nuicpp` and that you are the
`micu` user, with `sudo` for nginx and systemd.

This host runs dozens of unrelated sites. **Never restart nginx** — always
`sudo nginx -t && sudo systemctl reload nginx`, which is atomic and leaves other
vhosts serving. Never edit files under `/etc/nginx/conf.d/` other than
`nuicpp-rate-limit.conf`; the Cloudflare files there are shared by every site.

## Routine deploy

```bash
cd /home/micu/nuicpp
git pull                                   # if deploying from a remote
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
ctest --test-dir build --output-on-failure
scripts/migrate.sh                         # idempotent; safe to re-run
sudo systemctl restart nuigraph-studio.service
scripts/deploy_check.sh
```

The restart is safe under load: the service traps SIGTERM, stops accepting new
connections, and drains in-flight requests before exiting, so a save in progress
is not truncated. `journalctl` will show `stopped cleanly` before the unit goes
down. If that line is missing, systemd killed it after the timeout — investigate
before deploying again.

Only rebuild the WebAssembly shell when `nui-frontend/` changed; it needs the
Emscripten SDK and takes minutes:

```bash
scripts/build_nui_frontend.sh
```

That script installs both `public/nui/index.js` and `public/nui/index.wasm` and
**fails loudly if the `.wasm` is missing**. Do not work around that failure: the
loader without its module produces a blank page. The server's `nuiShellReady()`
check will fall back to the server-rendered editor rather than serve a broken
shell, so a failed frontend build degrades instead of breaking — but it is still
a failed build.

## First-time install

### 1. Database

```bash
sudo -u postgres createuser nuigraph_user --pwprompt
sudo -u postgres createdb nuigraph_studio -O nuigraph_user
scripts/migrate.sh
psql -d nuigraph_studio -f deploy/postgres/runtime_privileges.sql   # as owner/superuser
```

`003_randomize_legacy_slugs.sql` uses `gen_random_uuid()`, which is built into
PostgreSQL 13+. `pgcrypto` is **not** installed on this host, so do not add
migrations that call `gen_random_bytes()`.

### 2. Environment

`.env` lives at `/home/micu/nuicpp/.env`, mode `600`, git-ignored.

| Variable | Production value | Notes |
| --- | --- | --- |
| `APP_ENV` | `production` | |
| `APP_HOST` | `127.0.0.1` | Never bind publicly; nginx is the only ingress |
| `APP_PORT` | `18081` | Must match `proxy_pass` in the vhost |
| `APP_BASE_URL` | `https://nuicpp.micutu.com` | Used to build share URLs |
| `DATABASE_URL` | `postgresql://nuigraph_user:…@localhost/nuigraph_studio` | |
| `AUTH_USERNAME` | admin login name | Guest usernames are `guest_<hex>` and cannot collide |
| `AUTH_PASSWORD_HASH` | `pbkdf2_sha256$<iters>$<salt>$<hash>` | |
| `SESSION_SECRET` | 32+ random bytes, hex | See the rotation warning below |
| `PUBLIC_ACCESS` | `true` | Anonymous visitors may create diagrams |
| `MAX_NODES_PER_DIAGRAM` | `1000` | |
| `MAX_EDGES_PER_DIAGRAM` | `2000` | |
| `MAX_VERSIONS_PER_DIAGRAM` | `50` | Older snapshots are pruned |
| `MAX_IMPORT_BYTES` | `1048576` | nginx rejects >2 MiB before buffering |
| `MAX_DIAGRAMS_PER_GUEST` | `60` | Hard ceiling per guest session |
| `DB_POOL_SIZE` | `8` | Pooled connections; see sizing below |
| `PROJECT_ROOT` | `/home/micu/nuicpp` | Static file root |

**Rotating `SESSION_SECRET` logs everyone out and orphans every guest-owned
diagram**, because owner hashes are derived from it. Only the admin account
would retain edit access. Rotate deliberately, never as routine hygiene.

`DB_POOL_SIZE` must stay comfortably below PostgreSQL's `max_connections` minus
what the other projects on this host use. Eight is enough that 60 concurrent
requests were served using only two backends; raising it trades idle backends
for headroom under burst, not throughput.

### 3. systemd

```bash
sudo cp systemd/nuigraph-studio.service.example /etc/systemd/system/nuigraph-studio.service
sudo systemctl daemon-reload
sudo systemctl enable --now nuigraph-studio.service
```

The unit runs as `micu` with `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome=read-only`, `MemoryDenyWriteExecute`, an empty capability set, and
`RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`. If you add a feature that
needs to write outside `/tmp`, add a `ReadWritePaths=` line rather than
loosening `ProtectSystem`.

### 4. nginx

Two files, both versioned in this repo:

```bash
sudo cp deploy/nginx/nuicpp-rate-limit.conf /etc/nginx/conf.d/nuicpp-rate-limit.conf
sudo cp deploy/nginx/nuicpp.micutu.com.conf /etc/nginx/sites-available/nuicpp.micutu.com
sudo ln -sf /etc/nginx/sites-available/nuicpp.micutu.com /etc/nginx/sites-enabled/nuicpp.micutu.com
sudo nginx -t && sudo systemctl reload nginx
```

`nuicpp-rate-limit.conf` must be in `conf.d/` because `limit_req_zone` is only
valid at `http` level; the vhost only references the zones. Copying the vhost
without it makes `nginx -t` fail with "unknown limit_req_zone".

The vhost depends on `$from_cloudflare_origin`, defined in the shared
`/etc/nginx/conf.d/cloudflare-origin-guard.conf`. That file already exists and
is used by ten other vhosts — do not duplicate or modify it.

### 5. TLS

```bash
sudo certbot --nginx -d nuicpp.micutu.com --agree-tos --no-eff-email
```

Certbot manages the `listen 443 ssl`, certificate, and port-80 redirect blocks
in the vhost. If you overwrite the vhost from this repo, re-check those lines
still match what certbot expects before reloading.

## Verifying a deploy

```bash
scripts/deploy_check.sh
curl -fsS https://nuicpp.micutu.com/health
curl -sS -o /dev/null -w '%{http_code}\n' https://nuicpp.micutu.com/         # 200
sudo journalctl -u nuigraph-studio.service -n 50 --no-pager
```

Confirm the origin guard still rejects direct traffic (this is the control that
keeps the Cloudflare WAF from being bypassed):

```bash
curl -sS -o /dev/null -w '%{http_code}\n' --resolve nuicpp.micutu.com:443:<origin-ip> \
  https://nuicpp.micutu.com/health     # expect 403
```

## Static assets and caching

Static files are served with `ETag` and `Cache-Control: no-cache`, so a deploy
is picked up on the next request via a cheap `304`. No manual cache bust is
needed.

One exception, historical: assets fetched before that header was added may still
sit in Cloudflare's cache with the old four-hour TTL. If a stale `styles.css` or
`editor.js` outlives a deploy, purge it once from the Cloudflare dashboard
(Caching → Configuration → Purge). This should not recur.

The service worker (`public/sw.js`) caches six shell assets under a `VERSION`
key. **If you change what the shell needs, bump `VERSION`** — the activate
handler deletes every cache whose key does not match, which is the only way old
entries are cleared from visitors' browsers.

## Rollback

```bash
cd /home/micu/nuicpp
git log --oneline -10
git checkout <good-sha>
cmake --build build --config Release -j "$(nproc)"
sudo systemctl restart nuigraph-studio.service
```

Migrations are forward-only and additive; none of them drop or rewrite columns,
so an older binary runs against a newer schema. The exception is
`003_randomize_legacy_slugs.sql`, which rewrote slug values — rolling the code
back does not restore old slugs, and old share links stay dead either way. That
was the point of the migration.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| 502 from nginx | `systemctl status nuigraph-studio`; the app is probably down or on the wrong port |
| 403 on everything | Origin guard: traffic did not arrive via Cloudflare, or CF is in DNS-only mode |
| 429 during normal use | Edge zones in `nuicpp-rate-limit.conf`, then the app limits in `ensureWriteRateLimit` |
| Blank editor page | `ls public/nui/index.wasm`; re-run `scripts/build_nui_frontend.sh` |
| "storage unavailable" | `journalctl` has the real `pqxx` message; the client is deliberately told nothing |
| CSS or JS looks stale | Hard-reload; then check `curl -I` shows `etag` and `cache-control: no-cache` |
| Guest lost edit rights | Expected after 7 days or a cookie clear; ownership is session-bound by design |

```bash
sudo journalctl -u nuigraph-studio.service -n 200 --no-pager
psql "$DATABASE_URL" -c '\dt'
ss -ltnp | grep 18081
sudo nginx -t
```

See `docs/SECURITY.md` for what each of these controls is actually protecting.
