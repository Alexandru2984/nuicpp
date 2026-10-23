# Incident-response tabletop — 2026-10-23

## Exercise record

- **Exercise ID:** `NGS-TTX-2026-01`
- **Date:** 2026-10-23
- **Format:** self-guided maintainer walkthrough against the checked-in runbooks
- **Participants by role:** VPS operator, incident lead and communications owner
  (the operator assumes all three roles until another person is assigned)
- **Environment:** documentation and repository evidence only; production systems
  were not changed
- **Result:** conditional pass, with six follow-up gaps assigned below
- **Next exercise due:** 2027-04-23, or earlier after a SEV-1 or material identity,
  sharing or deployment change

This record describes a fictional incident. Times, addresses and events below
are exercise injects, not claims about production activity.

## Objectives and guardrails

The exercise tested whether one operator can classify the incident, preserve
useful evidence, contain access without destroying evidence, decide when the
GDPR assessment clock begins, recover safely and name measurable follow-up work.
It also tested the documented correlation path from Cloudflare/nginx to the app
using `request_id`.

No live blocking rule, credential rotation, service restart, database query or
notification was performed. Offsite backup and restore were explicitly out of
scope because that work remains deferred; the exercise does not represent the
missing backup control as healthy.

## Starting conditions

- NuiGraph is publicly reached through Cloudflare and nginx, while the app binds
  to loopback.
- Prometheus metrics and alert rules exist in the repository, but the live
  collector and Alertmanager receiver are not yet verified. The scenario
  therefore begins with an operator report rather than pretending paging works.
- One stateless administrator session can access every diagram. There is no MFA,
  per-device session list or individual session revocation.
- `SESSION_SECRET` currently signs sessions and derives CSRF, guest ownership and
  rate-limit identities. Rotating it invalidates sessions but also orphans guest
  ownership unless the old key is retained offline for a controlled migration.

Post-exercise remediation now splits those purposes into four required keys.
This paragraph remains the historical starting condition that produced IR-006;
`SESSION_SIGNING_SECRET_PREVIOUS` is the bounded planned-rotation window and
`tests/check_key_rotation.sh` is the current technical verification.

## Scenario, timeline and decisions

Exercise time starts at 10:00 EEST on 2026-10-23.

| Time | Inject | Decision and evidence sought |
| --- | --- | --- |
| T+00 | The operator receives a Cloudflare abuse report showing a sharp increase in requests to `/login`. | Open incident `NGS-IR-EXERCISE-01`; preserve the report and record the first-known and last-known-good times. Do not rely on the unwired alert receiver. |
| T+05 | Redacted app events show repeated `admin_login=failed` records and nginx entries can be joined through `request_id`. | Treat as a blocked attack, provisionally SEV-3. Preserve narrow time slices and aggregate counts without copying source-address lists into chat or tickets. |
| T+08 | An `admin_login=succeeded` event appears. The operator confirms it was not their login. | Escalate to **SEV-1** because administrator compromise is now confirmed. Assign incident and communications ownership, begin 15-minute updates and record controller awareness of a possible breach. |
| T+12 | DB-pool timeouts rise and `/ready` fails twice while `/live` remains healthy. | Treat database saturation as part of the incident until disproved. Preserve process, pool and database-session metadata; do not select diagram bodies. |
| T+18 | Redacted share-view events show access across several diagrams after the unauthorised login. The logs cannot prove which bodies were read. | Assume potential confidentiality impact. Start the personal-data breach assessment clock at 10:18 EEST instead of waiting for certainty. Scope affected rows and times using metadata only. |
| T+25 | The attacker continues to present a valid stateless admin cookie. | Put dynamic traffic into an edge/origin maintenance deny, including admin routes. `PUBLIC_ACCESS=false` alone is insufficient because it does not revoke a compromised administrator. Preserve evidence, then rotate the admin password and session material under the controlled recovery plan. |
| T+40 | Error rates return to baseline after containment. | Keep traffic closed. Validate release hashes, host integrity, database roles/sessions and the suspected entry path before deciding which credentials require rotation. |

The response deliberately uses the smallest reversible containment first. If host
compromise is suspected, the origin is isolated instead of being repaired in
place. Database, Cloudflare and hosting credentials are rotated only when their
exposure is possible; arbitrary rotation before evidence capture would increase
the outage and erase useful state.

## Evidence checklist

The incident lead records the collector, UTC timestamp, source and SHA-256 for
every exported artifact. Copies go to restricted incident storage; the original
logs remain read-only where practical.

- Cloudflare security events and configuration change history for the bounded
  incident window.
- nginx access/error slices and application journal slices correlated through
  `request_id`, with cookies, share capabilities and client addresses protected.
- Running release commit, executable/public-asset hashes, systemd unit state,
  process metadata, listeners, restarts and OOM evidence.
- PostgreSQL role attributes, grants, sessions, database size and aggregate row
  changes. Diagram titles and bodies are excluded unless the incident lead
  documents why content inspection is necessary.
- GitHub change, CI, dependency and deployment history for the running release.
- A decision log covering severity, containment, every credential considered,
  every command actually run and every person given evidence access.

## Communications and GDPR decision

The successful unauthorised administrator login plus cross-diagram access is
enough in this scenario to begin a documented personal-data breach assessment at
T+18. If notification is required, the supervisory-authority deadline is 72
hours after awareness: 2026-10-26 10:18 EEST for this fictional timeline. The
communications owner records data categories, approximate people/records,
likely consequences, mitigations and any reason for delayed notification.

Internal and external updates use `NGS-IR-EXERCISE-01`, time windows and
aggregate counts. They never include raw cookies, bearer links, diagram titles
or bodies, source-address lists or database connection strings. Legal advice is
required for a real notification decision and affected people are notified
without undue delay if the confirmed breach is likely to create high risk.

## Recovery criteria

Traffic remains closed until all applicable criteria are evidenced:

1. The entry point is patched or blocked, release and host integrity are
   established, and affected credentials are rotated in dependency order.
2. Compromised sessions no longer authenticate. If `SESSION_SECRET` is rotated,
   the previous key is kept offline only for an approved ownership migration and
   is never accepted concurrently by the public application.
3. PostgreSQL roles, active sessions, schema ledger, counts and health are
   plausible; `/live`, `/ready` and a private metrics scrape behave as expected.
4. Unit, browser, isolated integration, security, rate-limit, quota, migration,
   nginx and Prometheus-rule suites pass against non-production data.
5. A known operator exercises login, create, save, version, share and logout
   flows, then monitoring remains heightened for at least 24 hours.
6. Recovery evidence and the decision to reopen are attached to the incident
   record, with a rollback owner named.

## Findings and action register

| ID | Gap demonstrated | Priority | Owner | Due date | Verification |
| --- | --- | --- | --- | --- | --- |
| IR-001 | The checked-in alerts have no verified live collector or receiver, so initial detection depends on a human report. | P0 | VPS operator | 2026-11-06 | Send a labelled test alert and retain receiver evidence. |
| IR-002 | The emergency edge/origin maintenance deny is documented conceptually but has no rehearsed, rollback-safe procedure. | P0 | VPS operator | 2026-11-06 | Exercise it against staging or a disposable vhost and record timings. |
| IR-003 | Security logs are local and no retention/access policy or tamper-resistant copy is defined. | P1 | VPS operator | 2026-11-20 | Approve the policy and demonstrate restricted, integrity-checked export. |
| IR-004 | There is no breach-assessment template or maintained data inventory for fast affected-record estimates. | P1 | Data controller | 2026-11-20 | Review a completed blank template and inventory without user content. |
| IR-005 | A single stateless admin identity has no MFA, device list or individual session revocation. | P1 | Maintainer | 2026-12-15 | Negative authorization tests plus revoke-one-session and MFA tests pass. |
| IR-006 | At exercise time one `SESSION_SECRET` coupled revocation to guest ownership and rate-limit identity; remediated locally after the exercise. | P1 | Maintainer | 2026-12-15 | Split-key config, documented bounded rotation and disposable live continuity/retirement test pass. |

The deferred offsite-backup item remains a separate open High risk. It was not
tested, assigned a fictional success state or removed from the roadmap.

## Exercise result

**Conditional pass.** The runbook gave a sound severity, evidence, containment,
privacy and recovery sequence. Request correlation, redacted events, fail-closed
rate limits, readiness separation and the new metrics provide useful evidence.
Operational readiness is not complete until IR-001 and IR-002 are verified; the
session architecture keeps IR-005 and IR-006 material even after those fixes.
