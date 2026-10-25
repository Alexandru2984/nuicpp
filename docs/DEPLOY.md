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
runtime and test configs, installs the exact frontend lockfile, audits it,
ensures the pinned Chromium runtime is present, migrates an isolated `_test`
database, requires every live test to run, applies production migrations with
the owner role, reapplies the DML-only runtime grants, and then publishes a
root-owned release under `/opt/nuigraph-studio/releases/`. The `current` symlink
changes atomically. A failed restart or health check restores the previous
symlink and restarts the previous release. Each release also contains read-only
native and npm CycloneDX inventories under `sbom/`; generation failure stops the
deploy before production migration.

On a fresh host, install Playwright's OS packages once after `npm ci`:

```bash
sudo nui-frontend/node_modules/.bin/playwright install-deps chromium
```

Routine deploys only verify/download the browser binary in the unprivileged
operator cache; they do not silently change host packages.

Monitoring is not installed implicitly by application deploy. After reviewing
the local-only trust boundary, wire the scrape and validated rules using
[`MONITORING.md`](MONITORING.md), configure a real Alertmanager receiver, and
send a test alert.

Review the release inventory when dependencies change:

```bash
node scripts/generate_sbom.mjs build/sbom
less build/sbom/nuigraph-native.cdx.json
less build/sbom/nuigraph-npm.cdx.json
```

The deploy regenerates these from the committed source and built binary. The
format, coverage and remaining provenance limitations are documented in
[`SUPPLY_CHAIN.md`](SUPPLY_CHAIN.md).

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
loader without its module produces a blank page. Production defaults to
`FRONTEND_SHELL=server`; selecting `nui` makes both `check-config` and application
startup fail until `index.html`, `index.js` and a regular (non-symlink) `.wasm`
artifact are present. Enabling Nui also requires the reviewed
`'wasm-unsafe-eval'` CSP source in both the app and nginx candidate before the
vhost is installed.

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

`scripts/migrate.sh` serialises deployers with a PostgreSQL advisory lock and
applies each pending file in one transaction. It records the filename, SHA-256
checksum and timestamp in `nuigraph_meta.schema_migrations`, a schema the app
role cannot access. **Applied migration files are immutable.** A checksum change
is a deploy failure; create the next numbered migration instead. On the first
deploy after introducing the ledger, the existing idempotent migrations run once
and become the baseline.

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
| `APP_BASE_URL` | `https://nuicpp.micutu.com` | Bare public origin only (no credentials, path, query or fragment); used for share URLs and unsafe-request Origin validation |
| `DATABASE_URL` | `postgresql://nuigraph_app:…@localhost/nuigraph_studio` | Runtime role, never the owner role |
| `AUTH_USERNAME` | admin login name | Guest usernames are `guest_<hex>` and cannot collide |
| `AUTH_PASSWORD_HASH` | `pbkdf2_sha256$<iters>$<salt>$<hash>` | |
| `SESSION_SIGNING_SECRET` | 32+ random bytes, hex | Current cookie-signing key |
| `SESSION_SIGNING_SECRET_PREVIOUS` | empty normally | Old signing key during a planned rotation, for 7 days maximum |
| `CSRF_SECRET` | distinct 32+ random bytes, hex | CSRF derivation only |
| `OWNER_HASH_SECRET` | stable distinct 32+ random bytes, hex | Guest ownership; see migration warning below |
| `RATE_LIMIT_SECRET` | distinct 32+ random bytes, hex | Pseudonymous limiter identities only |
| `PUBLIC_ACCESS` | `true` | Anonymous visitors may create diagrams |
| `FRONTEND_SHELL` | `server` | Use `nui` only with a complete WASM bundle and matching reviewed CSP |
| `MAX_NODES_PER_DIAGRAM` | `1000` | |
| `MAX_EDGES_PER_DIAGRAM` | `2000` | |
| `MAX_VERSIONS_PER_DIAGRAM` | `50` | Older snapshots are pruned |
| `MAX_IMPORT_BYTES` | `1048576` | nginx rejects >2 MiB before buffering |
| `MAX_DIAGRAMS_PER_GUEST` | `60` | Hard ceiling per guest session |
| `MAX_DIAGRAMS_TOTAL` | `5000` | Global row ceiling across every owner and creation path |
| `MAX_LOGICAL_BYTES_PER_GUEST` | `33554432` | 32 MiB of current diagrams plus retained snapshots per guest |
| `MAX_LOGICAL_BYTES_TOTAL` | `536870912` | 512 MiB global logical-content ceiling |
| `DB_POOL_SIZE` | `8` | Pooled connections; see sizing below |
| `PROJECT_ROOT` | `/opt/nuigraph-studio/current` | Root-owned release/static root |

All four active secrets must be distinct. For the first split-key deployment,
copy the old `SESSION_SECRET` value into both `OWNER_HASH_SECRET` and
`SESSION_SIGNING_SECRET_PREVIOUS`, generate fresh signing/CSRF/rate-limit keys,
and remove the legacy variable (the app rejects it). Equality between the
previous signing key and owner key is the sole transitional exception. Active
old cookies are re-signed without extending their original expiry on
`/api/session`; remove the previous signing key after seven days.
During a signing-key compromise, do not configure the previous key: immediate
revocation takes priority over session continuity.

The first HTTPS request to `/api/session` also upgrades a valid legacy
`ngs_session` to `__Host-ngs_session` without changing its identity or expiry,
then expires the old name. Seeing upgrade events immediately after deployment
is expected. Confirm the prefixed cookie has `Secure; Path=/`, no `Domain`, and
that secure logout emits expirations for both names before considering the
rollout complete. After the code has been deployed for at least seven days (the
longest session lifetime), schedule removal of the legacy read fallback; do not
remove it early and silently detach active guest-owned diagrams.

Never rotate `OWNER_HASH_SECRET` without a tested database re-key migration;
changing it directly makes existing guest sessions unable to match their rows.
Rotating `CSRF_SECRET` only requires clients to fetch a new session response.
Rotating `RATE_LIMIT_SECRET` resets pseudonymous identities while old rows expire
within 10 minutes. Generate each new value independently with
`openssl rand -hex 32` and keep the configuration mode `0600`.

`DB_POOL_SIZE` must stay comfortably below PostgreSQL's `max_connections` minus
what the other projects on this host use. Eight is enough that 60 concurrent
requests were served using only two backends; raising it trades idle backends
for headroom under burst, not throughput.

Keep `MAX_DIAGRAMS_TOTAL` above the current diagram count and
`MAX_LOGICAL_BYTES_TOTAL` above the current `logical_storage_usage.logical_bytes`
value. The latter counts current user-controlled diagram content and retained
snapshot JSON, not PostgreSQL indexes, WAL or page overhead; continue monitoring
the database and disk metrics too. Lower either ceiling only with an explicit
retention or deletion plan. Reads and deletes keep working at capacity. New
content returns `503` at global capacity or `403` at the current guest's logical
limit, and failed saves roll back their revision and snapshot atomically.

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
tests/check_nginx_config.sh
sudo cp deploy/nginx/nuicpp-rate-limit.conf /etc/nginx/conf.d/nuicpp-rate-limit.conf
sudo cp deploy/nginx/nuicpp.micutu.com.conf /etc/nginx/sites-available/nuicpp.micutu.com
sudo ln -sf /etc/nginx/sites-available/nuicpp.micutu.com /etc/nginx/sites-enabled/nuicpp.micutu.com
sudo nginx -t && sudo systemctl reload nginx
```

`nuicpp-rate-limit.conf` must be in `conf.d/` because `limit_req_zone` is only
valid at `http` level; the vhost only references the zones. Copying the vhost
without it makes `nginx -t` fail with "unknown limit_req_zone".

The candidate test builds a self-contained nginx configuration, substitutes
only ephemeral certificate/log paths, and runs `nginx -t` before either live
file is copied. It also asserts the ACME locations, Cloudflare origin guard,
request-ID forwarding and bearer-safe log format. The final live `nginx -t`
remains mandatory because shared Cloudflare/Certbot files are host state that CI
cannot reproduce. A failed final test must never be followed by reload.

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

The app computes one release digest over the complete `public/` tree and uses it
as `?v=` on shell assets. An exact current digest is immutable for one year at
browser and CDN layers; unversioned, stale and invented values use `ETag` plus
mandatory revalidation. `/sw.js` stays `no-store` at all three layers,
registration uses `updateViaCache: none`, and its versioned registration URL
changes with the same digest.

Cloudflare was observed rewriting `/sw.js` to `Cache-Control: max-age=14400`.
Origin headers cannot override a matching Cache Rule. In the Cloudflare zone,
configure rules for `nuicpp.micutu.com` so Browser Cache TTL respects the origin,
Edge Cache TTL respects origin cache control, and `/sw.js` bypasses cache. Purge
the hostname's existing cached assets once after changing the rules. Then verify:

```bash
curl -sSI https://nuicpp.micutu.com/sw.js | grep -Ei 'cache-control|cf-cache-status'
# expect Cache-Control: no-store, max-age=0 and DYNAMIC/BYPASS (never a 4h TTL)
curl -fsS https://nuicpp.micutu.com/ | grep -oE '/styles[.]css[?]v=[0-9a-f]{16}'
# request that exact URL and expect max-age=31536000, immutable
```

Cloudflare documents that Cache Response Rules and Edge/Browser TTL settings can
override origin `Cache-Control` and CDN-specific headers. Keep this dashboard
control in the release checklist; the repository cannot enforce account-level
rules. See Cloudflare's current
[CDN-Cache-Control precedence](https://developers.cloudflare.com/cache/concepts/cdn-cache-control/)
and [Origin Cache Control](https://developers.cloudflare.com/cache/concepts/cache-control/)
documentation before changing the rule.

The service worker caches ten shell/offline assets under a `VERSION` key.
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
was the point of the migration. `004_diagram_revisions.sql` only adds a defaulted
column, so an older binary ignores it during rollback, but the stale-write guard
is absent until the revised binary is restored. `005_persistent_rate_limits.sql`
adds an independent table; an older binary ignores it and falls back to its
process-local counters. Do not drop the table during rollback: doing so destroys
active throttling state and is unnecessary. Rolling back code never deletes
ledger rows or reverts SQL; use a new forward migration for any schema correction.
`006_explicit_sharing.sql` preserves existing links as enabled and defaults new
rows to private. An older binary ignores the flag and would make every stored
slug reachable again, so do not roll back across this migration without first
restoring the hardened binary or denying `/d/` and `/api/diagrams/slug/` at nginx.
`007_share_expiry.sql` preserves existing links with no deadline and adds only a
nullable timestamp. An older binary ignores that timestamp and would re-expose
expired links whose stored enabled flag remains true. Never roll back across
this migration while share routes are reachable; restore the hardened binary or
deny both `/d/` and `/api/diagrams/slug/` at nginx until it is running again.
`008_logical_storage_quota.sql` backfills per-row estimates and an atomic global
counter. Older binaries leave new/updated content estimates stale and can bypass
the byte ceiling. A rollback may serve reads and deletes, but must deny mutating
API methods until the quota-aware binary is restored; do not drop the counter or
its triggers.
`009_idempotent_creates.sql` adds only a retry ledger with a cascading foreign
key. An older binary ignores it and remains schema-compatible, but creation
retries can again produce duplicates while that binary runs. Keep the table: it
contains only domain-separated HMACs and deleting it is unnecessary for rollback.
The next hardened release can safely resume using unexpired rows.
`FRONTEND_SHELL` adds no database state: roll back with the prior binary while
keeping it at `server`, or restore the previous vhost and release together if a
Nui-specific CSP was introduced.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| 502 from nginx | `systemctl status nuigraph-studio`; the app is probably down or on the wrong port |
| 403 on everything | Origin guard: traffic did not arrive via Cloudflare, or CF is in DNS-only mode |
| 429 during normal use | Edge zones in `nuicpp-rate-limit.conf`, then active pseudonymous rows in `rate_limit_buckets`; do not clear a production bucket without confirming the caller and abuse signal |
| Login/write returns 503 | Check PostgreSQL readiness, pool saturation and the `rate_limit_storage` event; the abuse control deliberately fails closed |
| Nui mode refuses startup | Confirm `FRONTEND_SHELL=nui`, inspect regular files under `public/nui/`, then re-run `scripts/build_nui_frontend.sh`; never replace the gate with a symlink |
| "storage unavailable" | Correlate the response `X-Request-ID` with the generic `storage_failure` journal event and PostgreSQL logs; raw connection errors are deliberately not journaled |
| Save/delete/restore returns 412 | Another client won the revision; reopen or refresh, then retry deliberately |
| Save/delete/restore returns 428 | Client omitted the latest diagram ETag in `If-Match` |
| Migration checksum mismatch | Restore the committed historical file; put the intended change in a new numbered migration |
| CSS or JS looks stale | Check the Cloudflare Cache Rule above, purge once, then confirm the origin revalidation headers |
| Guest lost edit rights | Expected after 7 days or a cookie clear; ownership is session-bound by design |

```bash
sudo journalctl -u nuigraph-studio.service -n 200 --no-pager
sudo -u postgres psql -d nuigraph_studio -c '\dt+'
ss -ltnp | grep 18081
sudo nginx -t
```

See `docs/SECURITY.md` for what each of these controls is actually protecting.
