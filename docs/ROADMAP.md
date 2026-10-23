# Product, security and operations roadmap

Order matters: reliability and privacy foundations come before collaboration and
growth. Each item should ship as a small reviewed commit with tests, migration and
rollback notes where applicable.

## Completed locally through 2026-10-23

- GitHub Actions build and all live integration/security suites against a
  disposable PostgreSQL 18 database; ShellCheck, Cppcheck and npm audit run in
  the same fail-closed job.
- Separate scheduled CodeQL analysis for C++ and JavaScript, immutable action and
  PostgreSQL image references, and weekly Dependabot updates for Actions/npm.
- Canonical, description, Open Graph and Twitter metadata plus dynamic
  `robots.txt` and `sitemap.xml`; bearer share links, auth, probes, JSON API,
  offline shell and raw Nui assets are excluded from indexing.
- Critical Ubuntu PHP security backport `8.5.4-0ubuntu1.5` applied to the host and
  verified across both PHP-FPM pools and vhosts. This is host remediation, not an
  application release.
- Atomic global diagram ceiling across all owners and creation paths, with a
  concurrent multi-session PostgreSQL integration test and distinct `503`
  capacity response.
- Conditional save, delete and restore with monotonic revisions, strong ETags,
  `428` for missing preconditions, `412` for stale clients and browser-draft
  preservation.
- End-to-end request IDs in nginx and the app plus redacted structured events for
  login, logout, share views, authorization, CSRF, throttling, quotas, revision
  conflicts and storage failures.
- PostgreSQL-backed GCRA limits for login, writes and creation, using
  pseudonymous bounded keys; counters are atomic across instances, survive
  restart and fail closed when their decision cannot be persisted.
- Self-contained nginx candidate validation in CTest/CI, covering syntax, ACME,
  origin guard, request correlation, redacted capability logs and restrictive
  CSP before the production files can be copied.
- Content-derived release URLs and service-worker cache names across both editor
  shells, with immutable caching only for the exact current asset digest.
- Real Chromium regression coverage for CSP enforcement, offline navigation,
  service-worker cache replacement, API cache isolation and phone/desktop
  layout geometry.
- Explicit frontend-shell selection: the complete server-rendered path is the
  production default with a tighter CSP, while Nui mode fails configuration and
  release checks unless its HTML, loader and non-symlink WASM artifact exist.
- Loopback-only, no-store Prometheus telemetry with bounded labels for HTTP,
  latency, security, creation, readiness, storage and DB-pool signals, plus
  promtool-validated alert rules and response procedures. Live collector and
  Alertmanager wiring remains an operator step.
- First incident-response tabletop with a timed SEV-1 credential-compromise and
  data-access scenario, GDPR decision clock, evidence/recovery criteria and six
  owned follow-up actions. The result is conditional until live paging and the
  emergency maintenance deny are exercised.
- Nui pinned to its reviewed full upstream commit plus validated CycloneDX
  inventories for direct/native runtime components and the complete npm lock
  graph. CI retains commit-named artifacts and deploy installs read-only copies
  with each release.
- Searchable, keyboard-accessible command palette and a compact phone action bar
  with two locally persisted quick actions. The mobile layout now removes both
  scrolling toolbars, reserves space in zoom-to-fit and keeps coarse-pointer
  controls at least 44px; Chromium covers focus restoration and customisation.
- Browser-local architecture review with iterative cycle detection, orphan,
  endpoint, duplicate, self-loop, title and edge-label rules. Findings use safe
  text rendering and can centre their affected object; unit and Chromium tests
  cover deep graphs, the shipped asset and the offline cache.
- Private-by-default link sharing with an explicit accessible management dialog,
  128-bit capability rotation and revocation, optimistic revision checks,
  redacted lifecycle events, private visitor forks and cross-session negative
  authorization tests. Existing links remain active through migration for
  compatibility.
- Bounded browser-local node navigator with title/type/key search, direct canvas
  centring, `/` shortcut, command-palette and mobile-action access, safe text
  rendering, focus restoration and Chromium interaction coverage.
- Undoable multi-node alignment on six axes and horizontal/vertical equal-centre
  distribution through the command palette and customisable mobile actions. A
  pure local geometry engine has deterministic generated tests, finite-number
  guards and live Chromium coverage.

The latest application commits remain local until explicitly pushed and deployed.
Offsite backup work is intentionally deferred by the operator; its P0 item and
High risk remain open.

## P0 — contain current production risk

- Add encrypted off-host PostgreSQL backups, retention, failure alerts and a
  scheduled restore drill with measured RPO/RTO.
- Install the checked-in nginx candidate to reconcile production drift. ACME,
  origin-guard, correlation and redacted-log invariants now pass an isolated
  `nginx -t` in CTest/CI before either live file can be copied; the final live
  `nginx -t` and reload remain an explicit operator step.
- Correct the account-level Cloudflare cache rule and purge stale objects;
  content-versioned URLs, origin headers and service-worker updates are now
  deterministic unless a Cache Rule overrides them.
- Wire the checked-in Prometheus scrape/rules to the host collector and a tested
  Alertmanager receiver. Backup freshness remains deferred with the offsite
  backup work and must not be represented by a fake success signal.

## P1 — privacy, identity and trustworthy sharing

- Add an explicitly indexed `public` visibility distinct from the implemented
  private and unlisted-link states.
- Add share expiry plus optional password-protected and time-limited links.
- User accounts with email verification or OIDC, passkeys/WebAuthn and MFA for
  the administrator; convert a guest workspace into an account without data loss.
- Server-side admin sessions with device list, last-used metadata and individual
  revocation. Use a `__Host-` cookie and split signing, CSRF and stable ownership
  keys.
- Workspaces, invitations and roles (`owner`, `editor`, `viewer`) with a central
  policy layer and negative authorization tests.
- Trash/soft delete, restore window, permanent-delete workflow and retention
  policy.
- GDPR data export/delete, privacy notice, retention controls, subprocessor list
  and audit-log access policy.
- Add signed/SLSA release provenance and reproducible-build metadata. Observe and
  triage the first remote SBOM CI run after push.

## P2 — editor quality and responsive UX

- Inline node/edge label editing, multi-select inspector and batch property edits.
- Rulers, configurable grid and selection framing.
- Orthogonal/smart edge routing, ports/anchors, curved connectors, waypoints and
  collision avoidance.
- Groups, frames, layers, lock/hide controls, reusable components and multiple
  pages per document.
- Expanded shapes: actor, queue, cache, container, document, cloud/network,
  Kubernetes/AWS/Azure/GCP libraries and custom SVG icons with safe sanitisation.
- Rich text/Markdown notes, links, attachments and accessible alternative text.
- Import Mermaid, draw.io and Graphviz; export PDF, transparent PNG, selected
  region, embeddable SVG and print layouts.
- Better history: named releases, visual canvas diff, compare any two versions,
  selective restore and duplicate-as-branch.
- Autosave to the server with visible sync state, offline mutation queue and
  conflict-resolution UI.
- WCAG 2.2 AA pass: keyboard canvas navigation, screen-reader object tree,
  high-contrast/forced-colour mode, focus trapping for sheets/dialogs and
  accessible status announcements.
- Romanian plus English localisation, locale-aware dates and RTL-ready layout.
- Cross-browser visual and interaction tests at phone, tablet, desktop and
  coarse-pointer breakpoints.

## P3 — collaboration and platform

- Optimistic document revisions first, then WebSocket live collaboration with
  presence, cursors, selections and conflict-safe CRDT/operation history.
- Comments, mentions, review/approval states and an activity feed.
- OpenAPI specification, scoped API tokens, idempotency keys, webhooks and an
  integration playground.
- Background job queue for heavy imports/exports, thumbnails and cleanup; object
  storage for attachments and generated artifacts.
- Admin console for usage, storage, abuse investigation, share revocation,
  retention and account/workspace controls.
- Per-workspace quotas, fair-use controls, Turnstile on suspicious anonymous
  creation, moderation/reporting and automated stale-guest cleanup.
- PostgreSQL query/index observability, connection-pool metrics, tracing and SLO
  dashboards.
- Progressive delivery with staging, migration rehearsal, canary health and
  automatic rollback based on readiness/error budget rather than one health call.

## P4 — differentiated product features

- Expand architecture linting with trust-boundary policy, naming conventions,
  configurable rule sets and CI/SARIF report export.
- Diagram-as-code round trips, Git sync and reviewable semantic diffs.
- Template marketplace with signed/curated templates and organisation libraries.
- Presentation mode, guided walkthroughs, comments-to-tasks and share analytics
  that preserve visitor privacy.
- Pluggable layout engines, large-graph virtualisation and server-side rendering
  for previews/social cards.
- Optional AI assistance only behind explicit consent: describe-to-diagram,
  layout suggestions, label cleanup and architecture review, with strict data-
  handling controls and no silent vendor transfer.

## Definition of done for every stage

- Threat model and authorization impact reviewed.
- Unit, integration, negative-security and responsive browser tests pass without
  using production data.
- Migration is forward-compatible and rollback behaviour is documented.
- Metrics, alerts, logs and privacy impact are defined.
- Documentation and runbooks match the deployed configuration.
- Commit contains no secrets and no unrelated working-tree changes.
