# Security audit and threat model

**Assessment date:** 2026-10-21  
**Scope:** application source, frontend assets, migrations, deployment scripts,
checked-in nginx/systemd configuration, the running service, and non-sensitive
production metadata. Diagram content and secret values were not inspected.

## Executive verdict

**MITIGATE THEN SHIP.** No unauthenticated remote-code execution, SQL injection,
stored XSS, committed production secret, or direct-origin bypass was found. The
service has a strong baseline: parameterised SQL, owner checks, CSRF protection,
bounded request sizes, a dedicated runtime user and database role, Cloudflare-only
public ingress, and a heavily sandboxed systemd unit.

The largest residual risks are operational and architectural rather than a single
critical exploit. Anonymous sessions can be renewed indefinitely, historical and
error logs may retain bearer share identifiers, there is no alerting or
NuiGraph-specific backup/restore evidence, and sharing is always-on. Do not
expand public usage until the P0 controls in `ROADMAP.md` are complete.

## Evidence snapshot

- `main` was clean and three commits ahead of `origin/main` at assessment start.
- Release build and 42 C++ tests passed. JavaScript syntax checks and ShellCheck
  passed. Cppcheck found style findings only.
- `npm audit --package-lock-only --audit-level=low` reported zero known
  vulnerabilities.
- The initial audit found three database-backed CTest entries falsely reported
  as passed after skipping, while deploy pointed them at production. They now
  report **Not Run** locally, fail closed during deploy, and require both
  `APP_ENV=test` and connected database/role names ending in `_test`.
- No private key, credential URL, or production secret was found in tracked files
  or Git history. Fixed test hashes produced the only secret-pattern matches.
- The production process runs as `nuigraph:nuigraph`, listens on
  `127.0.0.1:18081`, and had no restart or recorded HTTP 5xx event in the sampled
  journal. `systemd-analyze security` rated its exposure `1.3 OK`.
- The PostgreSQL runtime role was non-superuser, had no role-creation, database-
  creation or RLS-bypass privilege, and owned no database objects.
- Public responses included CSP, HSTS, frame, MIME-sniffing, referrer, permissions,
  COOP and CORP headers. A request to the host interface outside Cloudflare was
  rejected with 403. Loopback is intentionally trusted for local/tunnel traffic.
- The live vhost passes `nginx -t`, but differs from the repository copy in its
  ACME challenge handling. Copying the repository version verbatim can regress
  certificate renewal.
- Cloudflare returned `Cache-Control: max-age=14400` for `sw.js`, overriding the
  application's `no-cache` intent and allowing a stale service worker for four
  hours.
- The deployed `public/nui` directory contained no `.wasm`; production therefore
  uses the server-rendered fallback even though the project describes the Nui/WASM
  shell as the primary editor.
- The database contained 15 diagrams, 60 nodes, 45 edges and 16 versions at the
  time of the metadata-only check. It occupied about 8 MiB.
- Package metadata was refreshed on 2026-10-20. Security updates were pending for
  host packages including Git and `libxtst`; none of the directly linked OpenSSL,
  libpq, cpp-httplib or nginx packages appeared in the pending list. Cloudflared
  also had a newer package available.

## STRIDE threat model

| Rank | STRIDE | Scenario | Likelihood | Impact | Existing controls | Required treatment |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Denial of service | An attacker rotates guest cookies and source addresses, creates large diagrams and snapshots, or saturates DB workers/storage. App limits reset on restart and there is no global retained-data ceiling. | High | Medium | Cloudflare/nginx/app limits, payload caps, pool cap, atomic per-session quota | Global storage budget; persistent/distributed limiting; Turnstile for suspicious creation; storage and latency alerts |
| 2 | Information disclosure | App, DB, backups, journals, or bearer-link logs expose private diagram content. Every diagram receives a share capability even before its owner intentionally shares it. | Medium | High | Random slugs, owner checks, least-privilege DB role, CSP, origin guard | Opt-in sharing with rotate/revoke; 128-bit tokens; redact tokens; encrypted and access-controlled backups; retention policy |
| 3 | Spoofing / elevation | Theft or forgery of the single admin session grants access to all diagrams. Stateless sessions have no individual revocation and one secret also derives guest ownership. | Low | High | HMAC cookies, Secure/HttpOnly/SameSite, PBKDF2, login throttles | `__Host-` cookie; server-side admin session revocation; MFA/passkey; split signing and ownership keys; alert on admin login |
| 4 | Tampering | Concurrent saves overwrite one another or collide while assigning `MAX(version)+1`; no optimistic revision check warns the user. | Medium | Medium | Transactions and bounded version history | Row locking now; later add revision/ETag conflict detection and recovery UI |
| 5 | Repudiation | Current access lines lack request IDs, actor type and security event records; there is no tamper-resistant audit trail. | Medium | Medium | systemd journal and nginx logs | Structured security events, request IDs, retention/access policy and alerts |
| 6 | Supply chain | C++ packages come from the OS without an SBOM; Nui is pinned to a mutable Git tag; CI does not continuously scan/build the release. | Medium | Medium | npm lockfile and audit; root-owned releases | Pin immutable commit, SBOM, CI scans, dependency update policy and signed release provenance |

## Blast radius and FAIR planning estimate

A full application-host compromise exposes every live diagram and version, the
database credential, admin password hash, and session signing/ownership key. That
key permits forged admin sessions and makes all guest owner identifiers
recomputable. The current database blast radius is 15 diagrams; there is no
architectural upper bound on future global volume. The database does not contain
raw session cookies, but diagram bodies may contain arbitrary user-entered PII,
credentials, architecture details, or incident data.

The project has no revenue, data-classification, incident-cost, or traffic inputs
in the repository, so a defensible exact Annualised Loss Expectancy is impossible.
For prioritisation only, using a small self-hosted-service assumption:

| Scenario | Loss-event frequency | Loss magnitude | Illustrative ALE |
| --- | ---: | ---: | ---: |
| Storage/availability abuse | 1–4/year | EUR 250–2,000 | EUR 250–8,000/year |
| Diagram disclosure | 0.1–0.5/year | EUR 2,000–20,000 | EUR 200–10,000/year |
| Admin takeover | 0.05–0.2/year | EUR 5,000–30,000 | EUR 250–6,000/year |

These are explicit assumptions, not accounting results. Replace them after
classifying stored data, measuring traffic, and assigning recovery/legal labour
costs.

## Detailed findings

### High

1. **No verified backup and restore path.** No NuiGraph-specific backup timer or
   restore drill was found. A database or operator failure can permanently remove
   every diagram. Add encrypted backups, retention, off-host replication, restore
   tests and ownership for failed jobs.
2. **Sharing is always-on and bearer-only.** Every diagram gets a share slug on
   creation. There is no explicit publish state, revocation or token rotation.
   Make diagrams private by default and expose a deliberate share lifecycle.

### Medium

1. **Renewable anonymous identity.** Quota enforcement is now atomic per owner,
   but clearing the cookie creates a fresh owner and quota. There is no global
   retained-data ceiling.
2. **Last-write-wins saves.** Version assignment and mutations now serialize on
   the diagram row, preventing version collisions, but a stale editor can still
   overwrite a newer save without a revision-conflict response.
3. **Bearer capabilities in historical/error logs.** Application and dedicated
   nginx access logging now redact `/d/{slug}` and
   `/api/diagrams/slug/{slug}`. Historical records and nginx error messages may
   still contain capabilities; restrict retention/access and rotate exposed
   links after the share-lifecycle feature lands.
4. **Detection gap.** Nearly 20,000 journal lines were present, but there are no
   alert rules or on-call target. A quiet disclosure has an effectively unlimited
   MTTD.
5. **Configuration drift.** The checked-in vhost now contains the live ACME
   handling, but production remains drifted until that candidate is validated and
   installed through an explicit nginx configuration release.
6. **CDN cache drift.** Live static responses contradict the documented caching
   contract. The origin now sends explicit CDN revalidation headers and
   `no-store` for the worker, but the observed Cloudflare rule can override them;
   correct that account-level rule and purge existing objects.
7. **Login limiting remains process-local.** Capacity exhaustion no longer makes
   new addresses untracked, but counters still reset on restart and are not shared
   by multiple app instances. Edge limits reduce distributed attempts.
8. **One secret serves three purposes.** Session signing, CSRF derivation and guest
   ownership share `SESSION_SECRET`. Rotation logs everyone out and permanently
   orphans guest diagrams.
9. **Supply-chain visibility is incomplete.** No SBOM, continuous scanner, or
    immutable Nui revision is present.

### Low

1. The app returns 403 for both missing and foreign diagram IDs, which is safe but
   inconsistent with some 404 paths and should be documented as anti-enumeration.

## Detection targets

| Signal | Rule | Target MTTD | Owner |
| --- | --- | ---: | --- |
| Data path outage | Two consecutive `/ready` failures or any 5xx rate above 2% for 5 minutes | 5 minutes | VPS operator |
| Storage abuse | Create 429 rate, database growth or diagram count exceeds its rolling baseline | 15 minutes | VPS operator |
| Admin attack | 5 failed admin logins from one IP or 20 globally in 10 minutes; any successful admin login from a new network | 5 minutes | VPS operator |
| Origin bypass | Any accepted public-origin request whose TCP peer is not Cloudflare/tunnel | Immediate | VPS operator |
| Backup failure | Backup timer fails or no verified artifact appears in 26 hours | 30 minutes | VPS operator |
| Secret/token exposure | Secret scan or bearer token appears in non-redacted application logs | Before merge / 15 minutes runtime | Maintainer |

Current MTTD is effectively unbounded for silent disclosure and bounded only by
manual journal inspection for most other scenarios.

## Regulatory and vendor scope

Treat GDPR as in scope whenever a diagram contains data about an identifiable
person or IP-address logs can be linked to a visitor. A controller must assess and,
where required, notify the supervisory authority within 72 hours after becoming
aware of a personal-data breach. Preserve evidence and start the clock in the
incident record; do not wait for perfect certainty.

External dependencies in scope are Cloudflare (DNS, proxy/WAF/tunnel/analytics),
the VPS/hosting provider, GitHub (source and Nui fetch), Ubuntu package mirrors,
npm registry, Let's Encrypt and any backup destination. The repository does not
record DPAs, subprocessor terms, data regions, security-review dates or deletion
commitments. PostgreSQL and nginx run locally but their packages remain supply-
chain dependencies.

## Positive controls to preserve

- Keep loopback-only binding, Cloudflare origin filtering and trusted-proxy
  address handling together; changing one without the others breaks rate limits.
- Preserve parameterised SQL and the diagram-id ownership check before every
  content read or mutation.
- Preserve CSRF on every mutation and `Secure`, `HttpOnly`, `SameSite=Strict`
  cookies.
- Keep application and edge security headers aligned and tested.
- Keep the root-owned atomic release, least-privilege runtime DB role, systemd
  sandbox and rollback-on-failed-health design.
- Continue avoiding user-controlled file paths and shell execution.
