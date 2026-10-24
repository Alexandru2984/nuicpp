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
ngs_session = <username>.<unix-expiry>.<HMAC-SHA256(SESSION_SIGNING_SECRET, "username.expiry")>
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

Session signing, CSRF derivation, stable guest ownership and rate-limit
pseudonymisation use four required, mutually distinct secrets. A planned signing
rotation may temporarily configure `SESSION_SIGNING_SECRET_PREVIOUS`; a valid
old cookie is re-signed with the current key on its next `/api/session` request.
The original expiry is preserved rather than extending authorization. Remove the
previous key after at most seven days. In a compromise, omit it to revoke old
cookies immediately. `OWNER_HASH_SECRET` must remain stable unless a database
ownership migration has been designed and tested.

## Authentication

- The admin password is stored as `pbkdf2_sha256$<iterations>$<salt>$<hash>`
  and verified with `PKCS5_PBKDF2_HMAC` plus a constant-time compare.
- Login attempts are tracked per IP: the ninth attempt in 10 minutes is blocked.
  A successful login clears that address's bucket. nginx additionally caps
  `/login` at 10 requests/minute.
- The bounded limiter preferentially retains keys closest to their limit when a
  source-address flood fills it. New addresses therefore cannot make login
  tracking fail open merely by exhausting the key map.

Share slugs are bearer capabilities, but new diagrams are private until their
owner explicitly enables one. Enable and rotate mint a fresh 128-bit token;
disable makes both share routes return `404`, and re-enable rotates again so a
revoked URL cannot revive. Owners can choose an allowlisted lifetime from five
minutes through 30 days or no automatic expiry; the browser defaults to seven
days. PostgreSQL `now()` gates page, API and fork lookups, so app-server clock
skew cannot make one route disagree with another. An expired link returns `404`,
is hidden from API responses and can only be re-enabled with a newly rotated
token. Existing URLs remain enabled without expiry during the additive migration
for compatibility. The application logger and the dedicated
nginx access format replace values in `/d/{slug}` and
`/api/diagrams/slug/{slug}` with `<redacted>` before writing them. Do not switch
the vhost back to the default `$request` access format, which contains the raw
path, or add the raw `Referer`, which can contain the share page URL.

nginx generates `$request_id`, forwards it as `X-Request-ID`, records it in the
dedicated access log and receives the same value in the response. Requests
without a valid ID get a cryptographically random one from the app. Only 8–64
characters from a restricted alphabet are accepted, so a forged header cannot
inject a log line. Application access, exception and security-event records
carry the ID.

Security events cover admin login outcomes, logout, session re-signing,
authorization/CSRF denials, write/create throttling, quota/capacity exhaustion,
revision conflicts, share-page views and storage failures. They record actor
class, redacted route, source IP and outcome, but never usernames, cookies, CSRF
values, passwords, request bodies or share capabilities. Source IPs are personal
data; journal and nginx retention/access still need an explicit privacy policy.

## Authorization

Every diagram row carries an owner hash:

```
owner_hash = HMAC-SHA256(OWNER_HASH_SECRET, "owner:" + session_username)
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
2. **A version snapshot is diagram content.**
   `GET /api/diagrams/{id}/versions/{versionId}` returns a whole past diagram,
   so it takes `ensureCanRead` on the *diagram* id, not the weaker "may list
   versions". The storage query also filters on `diagram_id` as well as the
   version id, so a caller allowed to read one diagram cannot walk into another
   one's history by guessing version numbers.
3. **Enabled share links are read-only by slug.** `GET /d/{slug}` and
   `GET /api/diagrams/slug/{slug}` serve a read-only view to non-owners. Slugs
   are the capability, so they must be unguessable: `003_randomize_legacy_slugs.sql`
   replaced the old sequential `-<timestamp>` suffix with 16 hex characters from
   `gen_random_uuid()`. New links use a 32-hex-character, 128-bit random suffix;
   older 64-bit links remain valid until rotated or disabled. The database
   query also requires `sharing_enabled=true` and either a null deadline or a
   future deadline;
   knowing an inactive or expired slug does not bypass revocation. Sharing
   mutations require owner authorization, CSRF and the current revision ETag.
   Durations are a closed allowlist and expiry changes increment the diagram
   revision. Do not reintroduce a predictable suffix or an application-clock
   check on only one access path.
4. **Forking does not weaken ID authorization.** The owner-only
   `POST /api/diagrams/{id}/duplicate` remains protected against IDOR. A visitor
   may instead fork only through `POST /api/diagrams/slug/{slug}/fork`, which
   first resolves an active capability, applies creation rate/quota controls,
   and creates a private copy owned by that visitor's session.

## CSRF

Every mutating JSON endpoint requires `X-CSRF-Token`, issued by `/api/session`:

```
csrf = HMAC-SHA256(CSRF_SECRET, session_cookie_value + ".csrf")
```

It is bound to the exact session cookie, so a token from one session is useless
with another. Verification requires a valid session first, then a constant-time
compare. Because the token lives in a header, `SameSite=Lax` plus the header
requirement means a cross-site form post cannot mutate anything.

Persistent rate-limit bucket keys use `RATE_LIMIT_SECRET`. Rotating it starts new
pseudonymous identities; old database rows become unreachable and expire within
their normal 10-minute window instead of containing reusable client data.

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
| App/DB | 240 writes / 10 min per pseudonymous IP key | `ensureWriteRateLimit` |
| App/DB | 40 creates / 10 min per pseudonymous IP key | `ensureWriteRateLimit` |
| App/DB | 8 login attempts / 10 min per pseudonymous IP key | `/login` |
| App | `MAX_DIAGRAMS_PER_GUEST` rows per owner hash | `PostgresStorage::createDiagram` |
| App | `MAX_DIAGRAMS_TOTAL` rows across all owners | `PostgresStorage::createDiagram` |
| App/DB | `MAX_LOGICAL_BYTES_PER_GUEST` retained logical bytes per owner hash | Every content/snapshot mutation |
| App/DB | `MAX_LOGICAL_BYTES_TOTAL` retained logical bytes globally | Every content/snapshot mutation |

Per-IP limits only slow storage growth; they do not bound it, and the per-owner
quota can be renewed by clearing the guest cookie. Every creation first obtains
a global transaction-scoped PostgreSQL advisory lock, counts retained diagrams,
and inserts in that same transaction. It then takes the owner lock and applies
the session quota. The fixed global-then-owner lock order avoids deadlocks, and
parallel requests or rotated sessions cannot race past either ceiling. The API
returns `503` when global capacity is exhausted and `403` when only the current
session is full. Reads and deletes remain available.

Logical usage includes the current user-controlled title, description, node,
edge and metadata bytes plus every retained snapshot and note. PostgreSQL
triggers maintain one counter in the same transaction as inserts, updates,
pruning and cascade deletion. That counter serialises competing storage growth;
the application checks the global and owner ceilings before commit, so a denied
write rolls back its content, revision, snapshot and counter together. The
logical metric deliberately excludes indexes, WAL, dead tuples and fixed row
overhead, so physical database/disk alerts and a retention policy remain
required. An attacker can still fill the bounded allowance and deny later
writes, but cannot turn cookie rotation into unbounded retained content.

Application buckets are PostgreSQL-backed GCRA counters. They survive restarts,
are atomic across multiple app instances, refill gradually, and fail closed with
`503` if their database decision cannot be made. Bucket keys are HMAC-SHA256
digests derived from the category and client address, so the table does not
contain raw IP addresses. It is hard-capped at 20,000 active keys; admission of a
new key serialises cleanup and capacity checking so a rotating-address flood
cannot make the table grow without bound. At capacity, previously unseen keys
are rejected until an existing bucket expires. Cloudflare and nginx remain the
cheaper first line of defence, because a database limiter is not a DDoS sink.

## Optimistic concurrency

Every diagram row has a positive, monotonically increasing `revision`. Responses
that contain the live diagram also expose a strong ETag in the form
`"diagram-<id>-r<revision>"`. Save, delete and restore require that exact value
in `If-Match`; a missing precondition returns `428`, while a stale revision
returns `412` plus the current revision without changing the diagram, nodes,
edges or version history.

The database update increments the revision in the same conditional `UPDATE`
that checks it, so two requests presenting one revision cannot both win. The
browser preserves the losing tab as a local draft and tells the user to reopen
before applying it. This prevents silent overwrite; it is not an automatic merge
or a real-time collaboration protocol.

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

## Browser-local diagram review

Diagram Review is a deterministic static asset with no network calls. It reads
the in-memory diagram without mutating it and checks structural properties in
O(V+E) time. Cycle detection uses iterative traversals so an allowed deep graph
cannot exhaust the JavaScript call stack. Finding labels and messages are added
to the dialog with `textContent`; diagram titles, keys and labels are never
treated as HTML. Selecting Focus only changes the local canvas selection and
viewport. No diagram content is sent to a third-party analysis service.

The node navigator follows the same browser-local boundary. Search terms are
matched only against the open diagram's in-memory titles, types and keys. Node
values are inserted with `textContent`, never interpreted as markup, and only
the first 100 controls are rendered while the full bounded node set is searched.

Alignment and distribution also run in a deterministic browser-local module
with no network calls. The pure geometry function rejects unknown modes and
non-finite coordinates, does not mutate its inputs and returns only key/x/y
updates. The editor looks those updates up with `Map`, avoiding object-prototype
keys, records one undo snapshot and then applies them to the selected nodes.

## Error handling

`sendStorageError()` distinguishes validation errors, whose messages are meant
for the caller, from `pqxx::failure`, which can embed SQL text or connection
strings. The latter is logged server-side and replaced with a generic
`storage unavailable`. Raw database and handler exception text is not written to
the application journal either, because it may contain connection details. Use
the request ID, PostgreSQL logs and bounded operational metadata to investigate.

## Response headers

Set by both nginx and the app (`applySecurityHeaders`), kept in step by hand:

```
Content-Security-Policy: default-src 'self';
  script-src 'self' https://static.cloudflareinsights.com;
  style-src 'self';
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

- The production `FRONTEND_SHELL=server` policy does not grant WebAssembly code
  generation. The optional Emscripten-compiled Nui shell requires a deliberate
  `'wasm-unsafe-eval'` addition to both application and nginx policy; it does not
  permit JavaScript `eval()` the way `'unsafe-eval'` would.
- The `cloudflareinsights` hosts are for the beacon Cloudflare injects at the
  edge; nothing in this repository references them. `'unsafe-inline'` is
  deliberately withheld, so the beacon's inline loader stays blocked. That costs
  some analytics fidelity and keeps the script policy meaningful.
- Scripts, event handlers and styles are all external; neither script nor style
  policy grants `'unsafe-inline'`. The SVG exporter serialises a detached style
  element into the downloaded file and does not insert it into the live page.
- **If you edit the CSP, edit it in both places.** Two `Content-Security-Policy`
  headers are enforced as their intersection, which is easy to misread as a bug.

## Caching

Two rules that are security-relevant, not just performance:

- JSON responses carry `Cache-Control: no-store`. They are per-session data.
- The service worker (`public/sw.js`) never touches `/api/`, `/login`,
  `/logout`, `/health`, `/live`, or `/ready`. The Cache API is origin-wide and
  outlives the session cookie, so caching those would hand one visitor's diagrams
  to the next person using the same browser profile. Only nine static
  shell/offline assets are cached.

At startup the app hashes every regular, non-symlink file under `public/` and
injects the first 64 digest bits into shell URLs, manifest/icon references and
the service-worker cache name. An exact `?v=<current digest>` receives a one-year
`immutable` policy at browser and CDN layers. Missing, stale or invented versions
keep `ETag` plus mandatory revalidation. The worker script is stricter: every
cache layer receives `no-store`, registration uses `updateViaCache: none`, and
its URL also changes with the release digest. Cloudflare Cache Rules can override
origin headers, so the matching account-level rule remains an operator control
documented in `DEPLOY.md`.

## Process hardening

- Binds `127.0.0.1` only; nginx is the sole path in.
- Startup rejects ambiguous booleans, malformed integers, unsafe resource
  bounds, non-HTTPS production URLs, relative static roots, session secrets
  below 256 bits, and malformed or under-costed PBKDF2 password hashes. Use
  `nuigraph-studio check-config <path>` as a deployment gate; it never prints
  secret values.
- systemd unit applies `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`,
  `ProtectHome`, and a restricted syscall/address family set.
- SIGTERM/SIGINT stop the listener and drain in-flight requests before exit, so
  a restart does not truncate a save. SIGPIPE is ignored.
- Production secrets live in root-owned mode-`0600` files outside the checkout.
  systemd copies the runtime file into a private per-service credential mount;
  the service UID cannot read the source file. The database owner credential is
  separate and available only to the root-run migration step.
- A dependency-free repository scanner runs in CTest and CI before release. It
  detects provider credentials, private-key markers, credential-bearing database
  URLs and high-entropy values assigned to secret-like names. Reports contain
  only the rule and source location; candidate values are always redacted.
- Database-backed release tests require `APP_ENV=test` and verify through
  PostgreSQL that both the connected database and role names end in `_test`.
  Deploy makes unavailable tests fatal; local CTest reports them as skipped.
  Migration files parse `DATABASE_URL` as data instead of executing environment
  files as shell.
- A pinned Chromium end-to-end gate observes actual CSP violations, proves that
  service-worker activation removes obsolete release caches, checks that no API
  response enters Cache Storage, performs an offline navigation, and exercises
  both desktop and coarse-pointer phone layouts. Command-palette focus return,
allowlisted mobile-action persistence, 44px touch targets and the complete
enable/rotate/revoke sharing dialog are checked in the same real browser. It
uses the same isolated-test database guard as the live API suites.
- `/metrics` accepts loopback collectors only, is independently denied to public
  clients by nginx, and exports fixed-label aggregate telemetry without actors,
  request IDs, share capabilities or diagram content. It stays readable during
  a database outage with `nuigraph_database_up 0`, so loss of the dependency is
  itself observable. Alert rules and response steps live in `MONITORING.md`.
- Schema migrations are serialised and transactional. An owner-only ledger
  records each filename and SHA-256 checksum; an applied file that changes makes
  deploy fail rather than silently rewriting schema history.
- Nui is fetched by a reviewed full Git commit rather than a mutable tag. Every
  successful CI/deploy build generates validated CycloneDX inventories for the
  direct/native runtime and the complete locked npm graph. Release copies are
  root-owned and read-only; they are inventories, not signed provenance. See
  `SUPPLY_CHAIN.md`.
- The Ubuntu `libpqxx 7.10` header documents a shared-library double-free on
  normal exit for its header-defined type-name strings. The service links the
  package-provided static `libpqxx.a` so these objects have one owner; OpenSSL,
  libpq and the remaining libraries stay dynamically linked for normal updates.

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
- **Rate limiting depends on PostgreSQL.** A database outage deliberately blocks
  login and writes instead of silently disabling abuse controls; reads may still
  work if their required database operations are available.
- **Admin is a single shared credential.** There is no MFA or tamper-resistant,
  per-action audit trail. Security events identify the admin actor class but not
  an individual person.

## Verifying the model

`tests/check_security_smoke.sh` starts a throwaway instance on a free port and
asserts the live behaviour: a fresh guest cannot enumerate existing diagrams,
cannot read one by id, cannot copy one via duplicate, cannot read one's version
snapshots, gets `400` rather than a crash on an out-of-range id, is refused
without a CSRF token, receives a request ID and emits redacted share/CSRF
security events. It also proves that private, expired and revoked slugs return
`404` across page/API/fork paths, expired capabilities rotate before re-enable,
rotated capabilities stay dead and a different guest cannot change sharing.
Each `403` has a
matching positive control on the visitor's own diagram, so a pass cannot come
from a route that is simply broken. `tests/check_rate_limits.sh` exercises
shared counters across two app instances and verifies that throttling survives
their restart; `tests/check_global_quota.sh` launches six distinct guest
sessions concurrently against one remaining global row slot.
`tests/check_logical_quota.sh` proves an oversized update rolls back content,
revision, snapshot and accounting, then races six unrelated sessions against one
logical-capacity slot and recomputes the counter from its source rows.

```bash
ctest --test-dir build --output-on-failure
```

The origin guard is nginx-level and is not covered by the suite. Verify it by
hand after touching the vhost:

```bash
curl -sS -o /dev/null -w '%{http_code}\n' --resolve nuicpp.micutu.com:443:<origin-ip> \
  https://nuicpp.micutu.com/ready   # expect 403
curl -sS -o /dev/null -w '%{http_code}\n' https://nuicpp.micutu.com/ready   # expect 200
```
