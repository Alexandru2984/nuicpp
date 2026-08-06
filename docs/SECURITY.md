# Security model

This document describes what NuiGraph Studio actually enforces, where it is
enforced, and what it deliberately does not protect against. It is written for
whoever has to change the code or the deployment later.

## What this application is

A single-process C++ HTTP service bound to `127.0.0.1`, behind nginx, behind
Cloudflare. It stores user-created diagrams in PostgreSQL. It runs with
`PUBLIC_ACCESS=true`, which means **anyone on the internet can create diagrams
without an account**. That decision drives most of the design below: the system
cannot rely on registration to keep abuse out, so it relies on quotas, rate
limits, and per-session ownership instead.

There is exactly one real account, the admin, defined in `.env`.

## Trust boundaries

```
internet → Cloudflare → nginx (:443) → app (127.0.0.1:APP_PORT) → PostgreSQL
```

- **Cloudflare** is the only permitted ingress. The vhost rejects any peer
  outside the Cloudflare ranges with `403`, so learning the origin IP does not
  let an attacker bypass the WAF and edge rate limits.
  See `$from_cloudflare_origin` in `/etc/nginx/conf.d/cloudflare-origin-guard.conf`.
- **nginx** terminates TLS, applies per-IP rate and connection limits, and sets
  the security headers.
- **The app** does not trust nginx to stay configured correctly. It re-applies
  every security header itself and re-derives the client IP defensively. The
  vhost strips the upstream copies (`proxy_hide_header`) so each header appears
  exactly once in the response.
- **PostgreSQL** is reached over a local socket with a least-privilege role; see
  `deploy/postgres/runtime_privileges.sql`.

### Client IP derivation

`clientIp()` reads the **last** entry of `X-Forwarded-For`, not the first. nginx
appends the peer it observed, and `/etc/nginx/conf.d/cloudflare-realip.conf`
rewrites that peer from `CF-Connecting-IP`. Leading entries are attacker
supplied and are ignored. Getting this backwards would let anyone reset their
own rate-limit bucket by sending a forged header.

## Identity and sessions

Sessions are stateless, signed cookies — there is no server-side session store.

```
ngs_session = <username>.<unix-expiry>.<HMAC-SHA256(SESSION_SECRET, "username.expiry")>
```

- Verification checks the expiry first, then compares the HMAC in constant time.
  A tampered username or expiry fails the signature.
- Admin sessions last 8 hours. Guest sessions last 7 days
  (`kGuestSessionSeconds`), short enough that an abandoned browser does not keep
  edit rights forever.
- `Secure` is set only when `X-Forwarded-Proto` is `https`. In production it is
  always set; over plain HTTP on localhost it is omitted so the cookie is usable
  for local testing. `HttpOnly` and `SameSite=Lax` are always set.
- Guest usernames are random values minted by `/api/session`. They are not
  user-chosen and cannot collide with `AUTH_USERNAME`, which is what separates
  guest from admin.

`SESSION_SECRET` is the single key behind sessions, CSRF tokens, and ownership
hashes. Rotating it logs everyone out **and orphans every guest-owned diagram**,
because owner hashes are derived from it. Rotate only deliberately.

## Authentication

- The admin password is stored as `pbkdf2_sha256$<iterations>$<salt>$<hash>`
  and verified with `PKCS5_PBKDF2_HMAC` plus a constant-time compare.
- Failed logins are tracked per IP: 8 failures in 10 minutes blocks further
  attempts. nginx additionally caps `/login` at 10 requests/minute.
- The failure map is pruned on every touch and hard-capped at
  `kMaxLoginFailureKeys = 10000`. Once full it refuses to grow rather than let a
  spray of source addresses drive memory use.

## Authorization

Every diagram row carries an owner hash:

```
owner_hash = HMAC-SHA256(SESSION_SECRET, "owner:" + session_username)
```

The raw session value never reaches the database, so a database dump does not
yield usable session cookies.

Two predicates gate access, and **both are applied by id, never by trusting the
caller's claim**:

- `canReadDiagramById()` — admin, or owner hash matches.
- `canEditDiagram()` — same rule; there is no read-only-share editing path.

Handlers use `ensureCanRead` / `ensureCanEdit`, which emit `403` on failure.

Two subtleties worth preserving:

1. **Duplicate is a read.** `POST /api/diagrams/{id}/duplicate` returns a full
   owned copy of the source diagram, so it must pass the *read* check. It
   previously did not, which let any visitor copy any diagram by id — a direct
   object reference vulnerability. Any future endpoint that emits diagram
   content must call `ensureCanRead` even if it looks like a write.
2. **Share links are read-only by slug.** `GET /d/{slug}` and
   `GET /api/diagrams/slug/{slug}` serve a read-only view to non-owners. Slugs
   are the capability, so they must be unguessable: `003_randomize_legacy_slugs.sql`
   replaced the old sequential `-<timestamp>` suffix with 16 hex characters from
   `gen_random_uuid()`. Do not reintroduce a predictable suffix.

## CSRF

Every mutating JSON endpoint requires `X-CSRF-Token`, issued by `/api/session`:

```
csrf = HMAC-SHA256(SESSION_SECRET, session_cookie_value + ".csrf")
```

It is bound to the exact session cookie, so a token from one session is useless
with another. Verification requires a valid session first, then a constant-time
compare. Because the token lives in a header, `SameSite=Lax` plus the header
requirement means a cross-site form post cannot mutate anything.

## Id parsing

Route regexes match `\d+` with no length bound. `parseIdParam()` rejects empty
input, anything over 18 characters, trailing junk, and non-positive values
before `std::stol` can throw `std::out_of_range` out of a handler. Handlers turn
a rejection into `400`. Use `ensureId` for any new numeric route parameter.

## Abuse control

Three independent layers, because each has a different failure mode:

| Layer | Limit | Where |
| --- | --- | --- |
| Edge | 120 req/min per IP on `/api/`, burst 40 | `nuicpp-rate-limit.conf` |
| Edge | 10 req/min per IP on `/login`, burst 5 | `nuicpp-rate-limit.conf` |
| Edge | 24 concurrent connections per IP | `nuicpp-rate-limit.conf` |
| App | 240 writes / 10 min per IP | `ensureWriteRateLimit` |
| App | 40 creates / 10 min per IP | `ensureWriteRateLimit` |
| App | 8 login failures / 10 min per IP | `loginBlocked` |
| App | `MAX_DIAGRAMS_PER_GUEST` rows per owner hash | `ensureCreateQuota` |

The per-owner quota exists because per-IP limits only slow storage growth; they
do not bound it. A visitor who waits between bursts could otherwise fill the
disk. The quota gives a hard ceiling per session and returns `403` with a
message telling the visitor to delete something.

`RateLimiter` itself is bounded at `kMaxTrackedKeys = 20000`. When full it
evicts by **hit count first, recency second** — deliberately, so that flooding
the limiter with fresh keys cannot evict and thereby reset the counter of a
client that is currently being throttled. `tests/test_validation.cpp` covers
both the expiry sweep and this property; if you change the eviction policy, that
second test is the one that matters.

## Input validation

Enforced in the validation layer before anything reaches SQL:

- Title, description, node/edge key, label, and colour lengths and character
  sets.
- `MAX_NODES_PER_DIAGRAM`, `MAX_EDGES_PER_DIAGRAM`, coordinate and size ranges.
- `MAX_IMPORT_BYTES` (1 MiB) on import, with nginx rejecting bodies over 2 MiB
  before buffering them.
- Import rejects malformed JSON and edges referencing nodes that do not exist.
- Slugs must match `[A-Za-z0-9-]{1,140}`; icon paths must match
  `[A-Za-z0-9._-]+`. There is no user-controlled filesystem path anywhere.

All SQL uses parameter binding (`tx.exec(sql, pqxx::params{...})`). There is no
string-concatenated SQL in the codebase, and no shell execution at all.

## Error handling

`sendStorageError()` distinguishes validation errors, whose messages are meant
for the caller, from `pqxx::failure`, which can embed SQL text or connection
strings. The latter is logged server-side and replaced with a generic
`storage unavailable`. Do not pass raw exception text to `errorJson` for
anything that can originate in the storage layer.

## Response headers

Set by both nginx and the app (`applySecurityHeaders`), kept in step by hand:

```
Content-Security-Policy: default-src 'self';
  script-src 'self' 'wasm-unsafe-eval' https://static.cloudflareinsights.com;
  style-src 'self' 'unsafe-inline';
  img-src 'self' data: blob:;
  connect-src 'self' https://cloudflareinsights.com;
  object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'
X-Frame-Options: DENY
X-Content-Type-Options: nosniff
Referrer-Policy: strict-origin-when-cross-origin
Permissions-Policy: geolocation=(), microphone=(), camera=(), interest-cohort=()
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Resource-Policy: same-origin
Strict-Transport-Security: max-age=31536000; includeSubDomains   (nginx only)
```

Notes:

- `'wasm-unsafe-eval'` is required by the Emscripten-compiled Nui shell. It does
  **not** permit `eval()` of JavaScript the way `'unsafe-eval'` would.
- The `cloudflareinsights` hosts are for the beacon Cloudflare injects at the
  edge; nothing in this repository references them. `'unsafe-inline'` is
  deliberately withheld, so the beacon's inline loader stays blocked. That costs
  some analytics fidelity and keeps the script policy meaningful.
- `style-src 'unsafe-inline'` is still needed by the editor's inline style
  attributes. Removing it is a worthwhile future change.
- **If you edit the CSP, edit it in both places.** Two `Content-Security-Policy`
  headers are enforced as their intersection, which is easy to misread as a bug.

## Caching

Two rules that are security-relevant, not just performance:

- JSON responses carry `Cache-Control: no-store`. They are per-session data.
- The service worker (`public/sw.js`) never touches `/api/`, `/login`,
  `/logout`, or `/health`. The Cache API is origin-wide and outlives the session
  cookie, so caching those would hand one visitor's diagrams to the next person
  using the same browser profile. Only the six static shell assets are cached.

Static assets use `ETag` + `Cache-Control: no-cache`, meaning "store, but
revalidate": an unchanged file costs a `304`, a changed one is picked up on the
next request. Before this existed, Cloudflare applied its own default and a CSS
deploy could take hours to reach visitors.

## Process hardening

- Binds `127.0.0.1` only; nginx is the sole path in.
- systemd unit applies `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`,
  `ProtectHome`, and a restricted syscall/address family set.
- SIGTERM/SIGINT stop the listener and drain in-flight requests before exit, so
  a restart does not truncate a save. SIGPIPE is ignored.
- Secrets live in `.env`, mode `600`, git-ignored, loaded at startup only.

## Known limitations

These are accepted, not overlooked:

- **Guest ownership is browser-bound.** Clearing cookies loses edit access to
  diagrams created in that session. There is no recovery path by design — there
  is no identity to prove.
- **No per-account isolation beyond the owner hash.** A visitor who copies their
  own cookie to another machine gets the same rights. That is intended.
- **No collaboration/WebSocket layer**, so no real-time authorization model is
  needed yet. Adding one will require re-deriving authorization per message, not
  per connection.
- **`style-src 'unsafe-inline'`** remains, as noted above.
- **Rate limiting is per process.** The service is single-instance; running two
  copies would double every app-level limit. The nginx limits would still hold.
- **Admin is a single shared credential.** There is no MFA and no audit trail of
  admin actions beyond the access log.

## Verifying the model

`tests/check_security_smoke.sh` starts a throwaway instance on a free port and
asserts the live behaviour: a fresh guest cannot enumerate existing diagrams,
cannot read one by id, cannot copy one via duplicate, gets `400` rather than a
crash on an out-of-range id, is refused without a CSRF token, and receives the
security headers. `tests/check_rate_limits.sh` exercises the limiter and
`tests/test_validation.cpp` covers the validation and eviction logic.

```bash
ctest --test-dir build --output-on-failure
```

The origin guard is nginx-level and is not covered by the suite. Verify it by
hand after touching the vhost:

```bash
curl -sS -o /dev/null -w '%{http_code}\n' --resolve nuicpp.micutu.com:443:<origin-ip> \
  https://nuicpp.micutu.com/health   # expect 403
curl -sS -o /dev/null -w '%{http_code}\n' https://nuicpp.micutu.com/health   # expect 200
```
