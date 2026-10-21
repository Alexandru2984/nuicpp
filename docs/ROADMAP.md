# Product, security and operations roadmap

Order matters: reliability and privacy foundations come before collaboration and
growth. Each item should ship as a small reviewed commit with tests, migration and
rollback notes where applicable.

## P0 — contain current production risk

- Add a global storage/diagram ceiling on top of the atomic per-owner quota.
- Add revision/ETag conflict responses on top of serialized per-diagram version
  assignment so stale editors cannot overwrite newer saves.
- Replace the login limiter's capacity fail-open behaviour and move abuse limits
  to a persistent/shared enforcement point.
- Redact bearer share slugs from app/nginx logs; add request IDs and explicit
  login/share/security events without storing secret values.
- Add encrypted off-host PostgreSQL backups, retention, failure alerts and a
  scheduled restore drill with measured RPO/RTO.
- Reconcile the checked-in and live nginx vhost, including ACME handling. Add
  `nginx -t` against the candidate config before installation.
- Correct the account-level Cloudflare cache rule and version static asset URLs;
  origin headers and service-worker updates are now deterministic unless a Cache
  Rule overrides them.
- Ship the actual WASM artifact or formally remove it from the primary path; fail
  release assembly when the chosen shell is incomplete.
- Add a real-browser regression test for CSP, offline navigation and worker
  replacement.
- Add monitoring/alerts for readiness, 5xx, latency, DB pool timeouts, create
  rates, disk growth, auth failures, restarts, certificate expiry and backups.
- Perform and record the first incident-response tabletop.

## P1 — privacy, identity and trustworthy sharing

- Private-by-default diagrams with explicit `private`, `unlisted` and `public`
  visibility.
- 128-bit share capabilities with create, expire, rotate and revoke actions;
  optional password-protected and time-limited links.
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
- Immutable dependency pins, SBOM, CI security scans and signed/reproducible
  release metadata.

## P2 — editor quality and responsive UX

- Command palette and a compact, customisable mobile bottom bar so core actions
  do not live in two horizontally scrolling toolbars.
- Inline node/edge label editing, multi-select inspector and batch property edits.
- Align/distribute actions, rulers, guides, configurable grid, zoom-to-fit,
  selection framing and navigator/search across nodes.
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

- Architecture validation rules and linting (cycles, orphan nodes, missing
  labels, trust-boundary checks).
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
