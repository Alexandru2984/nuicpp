# Security audit and threat model

**Assessment date:** 2026-10-21  
**Scope:** application source, frontend assets, migrations, deployment scripts,
checked-in nginx/systemd configuration, the running service, and non-sensitive
production metadata. Diagram content and secret values were not inspected.

**Remediation updates, 2026-10-22 through 2026-10-26:** CI, CodeQL, dependency automation and SEO
privacy controls were pushed; their remote CI, CodeQL and secret-scanning checks
passed. Later hardening remains local until pushed and deployed. The operator
explicitly deferred offsite backup work, so the corresponding High finding
remains open. The host's official Ubuntu PHP security backport was applied and
verified independently of the app deploy.
Application login/write/create limits are now PostgreSQL-backed, bounded and
tested across two processes and a full restart; deployment is still pending.
The browser shell now has a pinned Chromium gate covering CSP, offline fallback,
worker replacement, cache isolation and responsive phone geometry.
The phone layout replaces its scrolling toolbars with a bounded, locally
customisable action bar, and the searchable command palette restores focus and
traps keyboard navigation. Command labels are static text, persisted action IDs
are allowlisted, and Chromium covers desktop and phone interaction paths.
The server-rendered shell is now the explicit production default; selecting Nui
fails closed on an incomplete or symlinked bundle, and the default CSP no longer
grants WebAssembly code generation.
Aggregate Prometheus instrumentation and validated alert rules now cover the
documented detection targets without exporting actor or diagram data. Detection
remains ineffective until the host collector and a tested Alertmanager receiver
are wired during an explicit operational change.
The first incident-response tabletop produced a conditional pass: severity,
evidence, containment, GDPR timing and recovery decisions were workable, while
live paging, emergency traffic denial, log retention, breach inventory, admin
revocation/MFA and key separation received named owners and due dates.
Nui is now pinned to the full commit behind its reviewed release tag. Native,
linked Ubuntu and complete npm-lock CycloneDX inventories are validated in CTest,
retained by CI and installed read-only with a release; signing and provenance
remain open.
Architecture review now executes entirely in the browser with no external data
flow. Its linear, non-recursive graph traversal is covered beyond the configured
node limit, and all finding text is rendered as text rather than markup.
New diagrams are now private by default. Owners explicitly enable, rotate or
disable 128-bit bearer links through CSRF-protected, ETag-conditional endpoints;
revoked routes return `404`, re-enable never revives an old token, and lifecycle
events remain redacted. Existing links stay enabled through migration for
compatibility, and production retains the old behavior until deployment.
Large diagrams now have a bounded, browser-local node navigator that searches
in-memory titles, types and keys, renders untrusted values as text, restores
keyboard focus and reuses the reviewed canvas centring path.
Multi-node alignment and distribution now use a pure local geometry engine with
finite-number validation, prototype-safe key lookup and one-step undo. Generated
unit cases plus Chromium cover the invariants and user interaction.
Share links now support allowlisted lifetimes using the PostgreSQL clock on page,
API and fork paths. Expired capabilities return `404`; re-enable always rotates;
owner-only expiry changes remain CSRF- and ETag-protected; blocked-view spikes
have a bounded-label alert. Existing links intentionally migrate with no expiry.
Retained user content now has transactional per-guest and global logical-byte
ceilings in addition to row quotas. A trigger-maintained singleton serialises
growth across app instances; rejected writes roll back revisions and snapshots;
permanent delete/prune release capacity; usage and capacity have a validated alert.
Session signing, CSRF derivation, stable guest ownership and rate-limit
pseudonymisation now use distinct required keys. Planned signing rotation can
re-sign old cookies during an explicit bounded window; emergency rotation omits
the old key. A live restart test proves ownership continuity and key retirement.
Inline node and edge-label editing keeps user-controlled labels in input values and SVG
`textContent`; it does not parse markup. Browser coverage uses an active-event
payload and verifies that rename, display and undo never create an HTML element
or execute the payload.
All five diagram-creation paths now support PostgreSQL-serialized, 24-hour
idempotency. The ledger stores independent HMACs for principal, caller token and
request material rather than raw identities, keys, share slugs or bodies.
Conflicting reuse and malformed/duplicate headers are bounded security events
with a validated abuse alert. A live eight-way concurrency regression found and
fixed a connection-pool exhaustion deadlock, then proved one retained diagram,
one creation metric and exact replay behavior across blank, template, duplicate,
import and fork operations.
Deletion now crosses an explicit private trash boundary. Soft delete revokes
sharing atomically and blocks every normal content/history path; restore stays
private, permanent deletion is owner/admin-only, and all three transitions are
CSRF- and ETag-protected. Retained trash still consumes both quotas and has an
aggregate backlog metric; ten permanent deletions in ten minutes trigger a
validated critical alert with an evidence-preserving response procedure.
Automatic retention is now implemented behind a fail-closed two-key opt-in. A
database-clock coordination row admits one oldest-first bounded batch per global
interval across instances, and cascade/quota release is atomic. Production
activation remains intentionally blocked on an operator decision: without
verified recovery and live paging, the documented safe value is disabled.
Failure, missed-interval and unexpected-volume alerts plus an aggregate runbook
bound detection and containment without logging IDs, titles or owners.
The stage gate passed all 20 CTest suites against disposable PostgreSQL,
including the dedicated cross-session trash lifecycle and real Chromium
desktop/mobile accessibility flows; ShellCheck, Cppcheck, the repository secret
scan, OpenAPI drift validation, promtool and npm audit also passed.

## Executive verdict

**MITIGATE THEN SHIP.** No unauthenticated remote-code execution, SQL injection,
stored XSS, committed production secret, or direct-origin bypass was found. The
service has a strong baseline: parameterised SQL, owner checks, CSRF protection,
bounded request sizes, a dedicated runtime user and database role, Cloudflare-only
public ingress, and a heavily sandboxed systemd unit.

The largest residual risks are operational and architectural rather than a single
critical exploit. The finite global row/byte ceilings can still be filled to deny
new writes, historical and error logs may retain bearer share identifiers, and
there is no verified live alerting or NuiGraph-specific backup/restore evidence.
Do not expand public usage until the P0 controls in `ROADMAP.md` are complete.

## CISO gate: automatic trash retention (2026-10-26)

- **Threat model:** the top risk is Tampering through an incorrect/compromised
  retention configuration (low likelihood, high impact), followed by Repudiation
  of destructive activity (medium/high) and DoS from an unbounded trash backlog
  (high/medium). Two-key opt-in, a seven-day production floor, globally bounded
  oldest-first batches, aggregate evidence and alerts address the implementation
  risks; configuration-host compromise remains outside the app trust boundary.
- **Blast radius:** one committed run can remove at most the configured batch
  (default 50, hard maximum 500) of already-expired trash; repeated runs can
  eventually remove every eligible row, bounded globally by 5,000 retained rows
  and 512 MiB at defaults. A defensible FAIR annualised dollar loss cannot be
  calculated from this repository because incident frequency, recovery labour,
  revenue and per-user impact inputs are absent; activation must not invent one.
- **Detection:** checked-in rules target immediate detection of a purge error or
  50-row/10-minute spike and detection after two missed intervals plus two
  minutes. Current production MTTD remains effectively unbounded until a live
  collector, Alertmanager receiver and operator rota are verified.
- **Response:** the monitoring runbook contains containment and evidence steps,
  and the general incident runbook/tabletop covers maintenance deny and GDPR
  timing. Automatic deletion itself has not been tabletop-tested, and verified
  recovery is deferred.
- **Regulatory:** GDPR's 72-hour supervisory-authority decision window applies
  when an incident qualifies as a personal-data breach likely to risk people;
  contractual and local-law windows still require case-specific review. Soft
  delete is not erasure, and primary-row purge cannot prove deletion from other
  retained stores.
- **Supply chain:** no vendor, subprocessor, dependency or external data flow was
  added. PostgreSQL, the existing process and existing monitoring contract are
  the only components in the path.

**Verdict:** the disabled-by-default mechanism is safe to ship. Enabling it in
production is **BLOCKED** while offsite recovery and live paging remain deferred;
the repository-wide verdict stays **MITIGATE THEN SHIP**.

## Evidence snapshot

- `main` was clean and three commits ahead of `origin/main` at assessment start.
- Release build and 52 C++ tests passed. All twelve CTest suites, including the six
  live suites, passed against a disposable PostgreSQL 18 database with isolated
  `_test` identities. JavaScript syntax checks, ShellCheck and Cppcheck passed.
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
- The deployed `public/nui` directory contained no `.wasm`; production currently
  reaches the server-rendered fallback. The local release contract now makes
  that shell the declared default and rejects Nui mode unless its complete
  non-symlink bundle is present; deployment is still pending.
- The database contained 15 diagrams, 60 nodes, 45 edges and 16 versions at the
  time of the metadata-only check. It occupied about 8 MiB.
- Package metadata was refreshed on 2026-10-21. Ubuntu's critical PHP fix from
  [USN-8948-1](https://ubuntu.com/security/notices/USN-8948-1) was applied to all
  installed PHP 8.5 core/FPM packages (`8.5.4-0ubuntu1.5`). Both FPM pools,
  configs, sockets and PHP vhosts passed post-update checks. Eighty-two unrelated
  package updates remain outside this targeted change and require their own
  compatibility window.
- GitHub Actions defines a pinned build/test/audit pipeline with a disposable
  PostgreSQL service, ShellCheck, Cppcheck, npm audit and CodeQL; Dependabot covers
  action and npm revisions. CI, CodeQL and GitGuardian passed on the first pushed
  hardening revision. CodeQL action updates are now grouped so Dependabot cannot
  mix incompatible action versions across init and analysis steps.

## STRIDE threat model

| Rank | STRIDE | Scenario | Likelihood | Impact | Existing controls | Required treatment |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Denial of service | An attacker rotates guest cookies and source addresses, fills the global row/byte or active limiter-key allowance, or saturates DB workers/storage. | High | Medium | Cloudflare/nginx limits, bounded shared GCRA buckets, payload caps, pool cap, atomic per-session/global row and logical-byte quotas, explicit deletion plus opt-in bounded retention and trash telemetry | Turnstile for suspicious creation; approve retention only after recovery/paging; deploy and wire the checked-in capacity and latency alerts |
| 2 | Information disclosure | App, DB, backups, journals, or an enabled bearer link expose private diagram content. Legacy links stay active until deliberately expired, rotated or revoked. | Medium | High | Private-by-default creation, optional DB-clock expiry, explicit rotate/revoke, 128-bit slugs, owner checks, least-privilege DB role, CSP, origin guard | Deploy the lifecycle changes; review legacy links; encrypted and access-controlled backups; retention policy |
| 3 | Spoofing / elevation | Theft or forgery of the single admin session grants access to all diagrams. Stateless sessions have no individual revocation. | Low | High | Purpose-separated HMAC keys, bounded signing-key rollover, `__Host-` Secure/HttpOnly/SameSite cookies, PBKDF2, login throttles | Server-side admin session revocation; MFA/passkey; alert on admin login |
| 4 | Tampering | Concurrent editors attempt to overwrite, delete or restore over newer work. | Low | Medium | Atomic revision increments, strong ETags, mandatory `If-Match`, `412` rejection, bounded history and local draft preservation | Add a merge/recovery UI before real-time collaboration |
| 5 | Repudiation | A shared admin credential prevents attribution to one person, and local logs can be altered after host compromise. | Medium | Medium | Correlated nginx/app request IDs; structured actor-class login/share/auth/CSRF/quota/storage events; redacted routes | Central/tamper-resistant audit storage, retention/access policy, individual admin identities and alerts |
| 6 | Supply chain | A compromised package source or CI account introduces a malicious dependency or substitutes build output. | Low | Medium | Immutable Nui/action/service refs; npm lock integrity and audit; native plus npm CycloneDX inventories; CodeQL, Cppcheck, ShellCheck, GitGuardian and Dependabot; root-owned releases | Add signed release provenance and independently verified reproducible-build metadata |

## Blast radius and FAIR planning estimate

A full application-host compromise exposes every live diagram and version, the
database credential, admin password hash, and session signing/ownership key. That
key permits forged admin sessions and makes all guest owner identifiers
recomputable. The current database blast radius is 15 diagrams; after deployment,
the default application ceiling limits future volume to 5,000 diagram rows and
512 MiB of current plus historical logical content. Physical PostgreSQL storage
also includes indexes, WAL and tuple overhead and therefore remains disk-alert
bounded rather than quota-equivalent. The database does not contain raw session
cookies, but diagram bodies may contain arbitrary user-entered PII, credentials,
architecture details, or incident data.

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
2. **Sharing was always-on and bearer-only (remediated locally).** New diagrams
   are private; enable/rotate/revoke/expiry are owner-only, CSRF-protected and
   revision-conditional. Expiration uses the database clock on every capability
   path and cannot revive a token. Deployment and a one-time review of
   intentionally preserved no-expiry legacy links remain required before closing
   the finding.

### Medium

1. **Global capacity can be weaponised (byte growth remediated locally).** Row
   and logical-byte enforcement is atomic per owner and globally across current
   content and retained snapshots. Clearing the cookie no longer makes retained
   growth unbounded, but an attacker can consume the finite global allowance and
   deny new writes. Deploy the change and its capacity alert; add retention and
   cleanup, and require stronger proof for suspicious anonymous creation.
2. **Bearer capabilities in historical/error logs.** Application and dedicated
   nginx access logging now redact `/d/{slug}` and
   `/api/diagrams/slug/{slug}`. Historical records and nginx error messages may
   still contain capabilities; restrict retention/access and rotate legacy
   links whose exposure cannot be ruled out.
3. **Detection gap.** Bounded metrics, validated alert rules and response steps
   now exist locally, but no verified collector/Alertmanager receiver is wired
   and there is no on-call rota. Until installation and a test alert, a quiet
   disclosure still has an effectively unlimited MTTD.
4. **Configuration drift.** The checked-in vhost contains the live ACME handling
   and now passes a self-contained `nginx -t` plus static security invariants in
   CTest/CI. Production remains drifted until that candidate is installed and
   passes the host-wide final test before reload.
5. **CDN cache drift.** The app now derives versioned shell/worker URLs from the
   complete public asset tree, serves matching assets as immutable and keeps the
   worker `no-store`. The observed Cloudflare rule can still override that
   contract; correct the account-level rule and purge existing objects.
6. **One secret served four purposes (remediated locally).** Four required,
   distinct secrets now isolate session signing, CSRF, stable guest ownership
   and rate-limit pseudonyms. A bounded optional previous signing key upgrades
   cookies without changing the owner hash; incident rotation can fail closed by
   omitting it. Production configuration and the planned seven-day retirement
   still need to accompany deployment.
7. **Rate limiting adds a database dependency.** Shared GCRA buckets now survive
   restarts and are bounded at 20,000 active pseudonymous keys, but a distributed
   source flood can fill that allowance and make new identities fail closed.
   Cloudflare/nginx enforcement and database latency alerts remain necessary.
8. **Release provenance is incomplete.** CI and continuous scanners use
   immutable action/image/Nui revisions, and native plus npm SBOMs capture the
   actual build inventory. The SBOMs and release are not signed, no SLSA
   attestation is emitted, and reproducibility has not been independently tested.

### Low

1. The app returns 403 for both missing and foreign diagram IDs, which is safe but
   inconsistent with some 404 paths and should be documented as anti-enumeration.
2. Conflict recovery is deliberate but manual: the server preserves integrity
   and the browser preserves the draft, but users must reopen and reconcile it.

## Detection targets

| Signal | Rule | Target MTTD | Owner |
| --- | --- | ---: | --- |
| Data path outage | Two consecutive `/ready` failures or any 5xx rate above 2% for 5 minutes | 5 minutes | VPS operator |
| Storage abuse | Create 429 rate, database growth or diagram count exceeds its rolling baseline | 15 minutes | VPS operator |
| Admin attack | 5 failed admin logins from one IP or 20 globally in 10 minutes; any successful admin login from a new network | 5 minutes | VPS operator |
| Origin bypass | Any accepted public-origin request whose TCP peer is not Cloudflare/tunnel | Immediate | VPS operator |
| Backup failure | Backup timer fails or no verified artifact appears in 26 hours | 30 minutes | VPS operator |
| Secret/token exposure | Secret scan or bearer token appears in non-redacted application logs | Before merge / 15 minutes runtime | Maintainer |
| Idempotency misuse | 10 malformed or conflicting key events within 10 minutes for 2 minutes | 12 minutes | VPS operator |

Current MTTD is effectively unbounded for silent disclosure and bounded only by
manual journal inspection for most other scenarios.

## Regulatory and vendor scope

Treat GDPR as in scope whenever a diagram contains data about an identifiable
person or IP-address logs can be linked to a visitor. A controller must assess
and, where required by [GDPR Article 33](https://eur-lex.europa.eu/eli/reg/2016/679/ojv),
notify the supervisory authority within 72 hours after becoming aware of a
personal-data breach. Preserve evidence and start the clock in the incident
record; do not wait for perfect certainty.

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
