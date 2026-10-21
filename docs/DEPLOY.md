# Deploy runbook

Production host: the VPS. Public URL: `https://nuicpp.micutu.com`.
Everything below assumes the source checkout is `/home/micu/nuicpp` and that
you are the `micu` user, with `sudo` for nginx and systemd.

This host runs dozens of unrelated sites. **Never restart nginx** — always
`sudo nginx -t && sudo systemctl reload nginx`, which is atomic and leaves other
vhosts serving. Never edit files under `/etc/nginx/conf.d/` other than
`nuicpp-rate-limit.conf`; the Cloudflare files there are shared by every site.

## Routine deploy

```bash
cd /home/micu/nuicpp
git pull                                   # if deploying from a remote
./scripts/deploy.sh
```

The deploy refuses a dirty tree, builds the committed source, validates both
runtime and test configs, migrates an isolated `_test` database, requires every
live test to run, audits the frontend dependency lock, applies production
migrations with the owner role, reapplies the DML-only runtime grants, and then
publishes a root-owned release under `/opt/nuigraph-studio/releases/`. The
`current` symlink changes atomically. A failed restart or health check restores
the previous symlink and restarts the previous release.

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
sudo -u postgres createuser nuigraph_app --pwprompt
sudo -u postgres createuser nuigraph_test --pwprompt
sudo env ENV_FILE=/etc/nuigraph-studio/migrate.env scripts/migrate.sh
sudo -u postgres psql -v ON_ERROR_STOP=1 -d nuigraph_studio \
  -v app_user=nuigraph_app -v migration_owner=nuigraph_user \
  -f deploy/postgres/runtime_privileges.sql
sudo -u postgres createdb nuigraph_studio_test -O nuigraph_test
```

`nuigraph_user` owns the database, schema, tables and sequences and is used only
by the root-run migration step. `nuigraph_app` owns nothing, belongs to no role,
and receives only connect, schema usage, table DML and sequence usage/select.
The privilege script refuses to continue if those ownership and role-attribute
invariants are not true; revoking `CREATE` from an owner does not constrain it.
`nuigraph_test` owns only the disposable `nuigraph_studio_test` database because
the deploy must both migrate it and delete fixtures. Never grant it access to
`nuigraph_studio` and never reuse a production credential in the test config.

`003_randomize_legacy_slugs.sql` uses `gen_random_uuid()`, which is built into
PostgreSQL 13+. `pgcrypto` is **not** installed on this host, so do not add
migrations that call `gen_random_bytes()`.

### 2. Environment

Production has three root-owned mode-`0600` files under
`/etc/nuigraph-studio/`. `app.env` is copied into a private systemd credential
mount visible only to the service. `migrate.env` contains only the production
owner `DATABASE_URL`. `test.env` is a full app config that points to the isolated
`nuigraph_studio_test` database. The source `.env` is for local development only
and must not remain on the production checkout.

Create the test config from the checked-in placeholder, then replace every
placeholder before the first deploy:

```bash
sudo install -o root -g root -m 0600 deploy/test.env.example \
  /etc/nuigraph-studio/test.env
sudoedit /etc/nuigraph-studio/test.env
sudo ./build/nuigraph-studio check-config /etc/nuigraph-studio/test.env
```

The test guard checks the connected database and login role, not just the URL.
It requires `APP_ENV=test` and both PostgreSQL names to end in `_test`. Missing
or unavailable test infrastructure fails a deploy; a normal local CTest run
reports those suites as **Not Run**. Migration config is parsed as data and is
never sourced as shell code. Root-run validation, migration and test commands
also start with a clean environment and a fixed system executable path, so an
operator's exported variables cannot override the files being checked.

| Variable | Production value | Notes |
| --- | --- | --- |
| `APP_ENV` | `production` | |
| `APP_HOST` | `127.0.0.1` | Never bind publicly; nginx is the only ingress |
| `APP_PORT` | `18081` | Must match `proxy_pass` in the vhost |
| `APP_BASE_URL` | `https://nuicpp.micutu.com` | Used to build share URLs |
| `DATABASE_URL` | `postgresql://nuigraph_app:…@localhost/nuigraph_studio` | Runtime role, never the owner role |
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
| `PROJECT_ROOT` | `/opt/nuigraph-studio/current` | Root-owned release/static root |

**Rotating `SESSION_SECRET` logs everyone out and orphans every guest-owned
diagram**, because owner hashes are derived from it. Only the admin account
would retain edit access. Rotate deliberately, never as routine hygiene.

`DB_POOL_SIZE` must stay comfortably below PostgreSQL's `max_connections` minus
what the other projects on this host use. Eight is enough that 60 concurrent
requests were served using only two backends; raising it trades idle backends
for headroom under burst, not throughput.

### 3. systemd

```bash
sudo useradd --system --user-group --home-dir /nonexistent --shell /usr/sbin/nologin nuigraph
sudo install -d -o root -g root -m 0755 /opt/nuigraph-studio/releases
sudo install -d -o root -g root -m 0700 /etc/nuigraph-studio
sudo cp systemd/nuigraph-studio.service.example /etc/systemd/system/nuigraph-studio.service
sudo systemctl daemon-reload
sudo systemctl enable --now nuigraph-studio.service
```

The unit runs as dedicated `nuigraph`, receives the config through
`LoadCredential`, cannot see the checkout or deploy user's home, and sees only
the root-owned release as read-only. It denies non-loopback network traffic,
permits binding only TCP `127.0.0.1:18081`, has an empty capability set, a
syscall allowlist and explicit task/file/memory/core limits. If a future feature
needs persistent writes, give it a dedicated state directory rather than
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
sudo /opt/nuigraph-studio/current/nuigraph-studio check-config /etc/nuigraph-studio/app.env
curl -fsS https://nuicpp.micutu.com/live
curl -fsS https://nuicpp.micutu.com/ready
curl -sS -o /dev/null -w '%{http_code}\n' https://nuicpp.micutu.com/         # 200
sudo journalctl -u nuigraph-studio.service -n 50 --no-pager
```

Confirm the origin guard still rejects direct traffic (this is the control that
keeps the Cloudflare WAF from being bypassed):

```bash
curl -sS -o /dev/null -w '%{http_code}\n' --resolve nuicpp.micutu.com:443:<origin-ip> \
  https://nuicpp.micutu.com/ready     # expect 403
```

## Static assets and caching

Static files are served with `ETag`, mandatory browser revalidation and matching
`CDN-Cache-Control`/`Cloudflare-CDN-Cache-Control` headers. `/sw.js` instead
sends `no-store` at all three layers, and registration sets
`updateViaCache: none`.

Cloudflare was observed rewriting `/sw.js` to `Cache-Control: max-age=14400`.
Origin headers cannot override a matching Cache Rule. In the Cloudflare zone,
configure rules for `nuicpp.micutu.com` so Browser Cache TTL respects the origin,
Edge Cache TTL respects origin cache control, and `/sw.js` bypasses cache. Purge
the hostname's existing cached assets once after changing the rules. Then verify:

```bash
curl -sSI https://nuicpp.micutu.com/sw.js | grep -Ei 'cache-control|cf-cache-status'
# expect Cache-Control: no-store, max-age=0 and DYNAMIC/BYPASS (never a 4h TTL)
```

Cloudflare documents that Cache Response Rules and Edge/Browser TTL settings can
override origin `Cache-Control` and CDN-specific headers. Keep this dashboard
control in the release checklist; the repository cannot enforce account-level
rules. See Cloudflare's current
[CDN-Cache-Control precedence](https://developers.cloudflare.com/cache/concepts/cdn-cache-control/)
and [Origin Cache Control](https://developers.cloudflare.com/cache/concepts/cache-control/)
documentation before changing the rule.

The service worker caches eight shell/offline assets under a `VERSION` key.
**If you change what the shell needs, bump `VERSION`** — the activate handler
deletes every cache whose key does not match, which is the only way old entries
are cleared from visitors' browsers.

## Rollback

```bash
sudo ln -s /opt/nuigraph-studio/releases/<good-release> /opt/nuigraph-studio/.rollback
sudo mv -Tf /opt/nuigraph-studio/.rollback /opt/nuigraph-studio/current
sudo systemctl restart nuigraph-studio.service
curl -fsS http://127.0.0.1:18081/ready
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
| CSS or JS looks stale | Check the Cloudflare Cache Rule above, purge once, then confirm the origin revalidation headers |
| Guest lost edit rights | Expected after 7 days or a cookie clear; ownership is session-bound by design |

```bash
sudo journalctl -u nuigraph-studio.service -n 200 --no-pager
sudo -u postgres psql -d nuigraph_studio -c '\dt+'
ss -ltnp | grep 18081
sudo nginx -t
```

See `docs/SECURITY.md` for what each of these controls is actually protecting.
