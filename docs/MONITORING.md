# Monitoring and alert response

NuiGraph exports privacy-preserving Prometheus text metrics on `GET /metrics`.
The application accepts that endpoint only from loopback (or a loopback
`X-Real-IP` set by the trusted local proxy), and the checked-in nginx vhost adds
an independent `allow 127.0.0.1; allow ::1; deny all` boundary. Do not expose or
proxy this endpoint through Cloudflare.

The endpoint includes only bounded labels and aggregate values: HTTP method and
status class, latency buckets, fixed security outcomes, creation source, object
counts, configured row/logical-byte capacity, database bytes and pool state. It
never exports IP addresses, usernames, owner hashes, request IDs, diagram
titles/bodies or bearer share capabilities. Diagram, rate-limit and logical-byte
counts are exact; potentially much larger node, edge and version counts use
PostgreSQL's live-row estimates so the monitor does not repeatedly scan millions
of rows. Responses are `no-store` and excluded from indexing.

The bounded security series includes allowed/blocked share views, successful
enable/disable/rotate/expiry/password lifecycle changes, required/successful/
failed/rate-limited share-password unlocks, signing-key and legacy
cookie upgrades, rejected cross-site mutations, foreign request origins and
unsupported request media types, malformed idempotency headers and conflicting
idempotency-key reuse, plus successful move/restore/permanent-delete trash
transitions and guest export/delete activity. Runtime logs use the same outcomes but redact capabilities
and idempotency keys. A sudden blocked-view spike after a planned rotation is
expected; an unexplained sustained spike should be correlated with access logs
without recovering or printing the old token.

## Scrape configuration

Run the collector on the same host and scrape the loopback listener directly:

```yaml
scrape_configs:
  - job_name: nuigraph
    scrape_interval: 30s
    scrape_timeout: 5s
    static_configs:
      - targets: ["127.0.0.1:18081"]
```

Load `deploy/monitoring/nuigraph-alerts.yml` as a Prometheus rule file. The TLS
rule additionally expects a blackbox-exporter HTTPS target named
`https://nuicpp.micutu.com`; the disk rule expects node-exporter with `job="node"`.
Point Alertmanager at a receiver that is actually monitored by the VPS operator,
then send a test alert before treating this as production detection.

Validate every rule change with:

```bash
tests/check_prometheus_rules.sh
```

The test uses a local `promtool` when present and otherwise a digest-pinned
Prometheus container. CI installs `promtool`; release validation fails closed
when neither validator is available.

## Alert ownership and targets

The VPS operator owns every alert until an explicit on-call rota exists. The
rules target 2–5 minute detection for outages, database loss, 5xx spikes, pool
timeouts and authentication attacks; capacity, latency, growth, disk and TLS
signals intentionally require longer persistence to limit noise. Trash backlog
must remain both above 100 retained diagrams and above 25 percent for 24 hours.
When automatic retention is enabled, dedicated bounded metrics expose its age,
global interval, claimed success/error runs, removed-row total and last-success
timestamp. Failures, two missed intervals and 50 removals in ten minutes are
critical alerts; the VPS operator remains the owner until paging is wired.
Any guest delete-all is critical and ten exports in ten minutes is a warning;
these signals are intentionally aggregate and cannot identify an owner.
Ten failed or rate-limited share-password attempts in ten minutes sustain a
warning after two minutes. The signal deliberately cannot identify the link or
source; correlate it only with already-approved redacted edge evidence.

Backup freshness is intentionally not represented here while offsite backup
work is deferred. Do not create a constant-success placeholder: add an alert
only when a real artifact timestamp and restore verification signal exist.

## Target or database down

1. Compare `curl --noproxy '*' -fsS http://127.0.0.1:18081/live` with `/ready`.
2. Inspect the service status and correlate recent generic storage failures by
   request ID. Do not paste connection strings or diagram content into tickets.
3. Check PostgreSQL availability and pool pressure before restarting anything.
4. If a restart is necessary, preserve the journal slice first and confirm the
   `stopped_cleanly` event afterward.

## Elevated errors or latency

1. Split the symptom by 5xx ratio, p95 latency, DB readiness and pool timeouts.
2. Correlate the window with deploys, migrations, restarts and create traffic.
3. Use aggregate PostgreSQL activity and slow-query evidence; do not inspect
   user diagram bodies unless incident scope makes that necessary.
4. Roll back the atomic release if the alert began immediately after deploy and
   readiness or the external smoke check regressed.

## Database pool pressure

1. Check `nuigraph_db_pool_connections` and the timeout counter together.
2. Identify long-running database work and lock waits before increasing
   `DB_POOL_SIZE`; this host shares PostgreSQL with other services.
3. A rate-limit storage failure blocks writes by design. Restore DB health rather
   than bypassing the fail-closed control.

## Abuse or capacity pressure

1. Compare create rate, row capacity, logical-byte capacity, rate-limit bucket
   capacity, trash backlog and physical DB byte growth over the same window. Logical bytes
   cover retained user content; DB bytes also include indexes, bloat and system
   overhead.
2. Contain at Cloudflare/nginx first when the source pattern is clear. Do not
   delete rate-limit rows or user diagrams merely to silence an alert.
3. Preserve aggregate evidence and open an incident when global capacity can be
   exhausted or legitimate creation is being denied.
4. Do not raise a logical ceiling until disk headroom and recent growth are
   understood. Moving to trash deliberately retains current and historical bytes;
   only owner-confirmed permanent deletion releases them. Do not purge user trash
   merely to silence the backlog alert—apply an explicit retention decision.

## Authentication attack

1. Correlate the alert with redacted `admin_login` events and Cloudflare data.
2. Block abusive sources at the edge; do not weaken application throttling.
3. Treat any unexplained successful login near the attack window as a potential
   credential incident and follow `docs/INCIDENT_RESPONSE.md`.

## Cross-site mutation attempts

1. Confirm the increase is the bounded `fetch_metadata_rejected/blocked` series
   and correlate request IDs with redacted application/access logs.
2. Split attempts by route shape and edge metadata. Do not log or promote raw
   `Sec-Fetch-Site` values into labels; the decision is already fail-closed.
3. A burst against `/login` or write routes usually indicates CSRF probing or a
   malicious embedded form. Contain a clear source pattern at Cloudflare while
   preserving evidence; do not disable CSRF or the Fetch Metadata firewall.
4. If a legitimate client is affected, verify whether it incorrectly forges a
   browser-reserved header. Non-browser clients should omit it, not claim a
   foreign browser context.

## Foreign request origins

1. Confirm the bounded `origin_rejected/blocked` series and correlate route
   shapes and request IDs without recording the raw Origin value.
2. Check `APP_BASE_URL` against the actual public scheme, hostname and port. It
   must be the bare canonical origin, not a path or internal proxy address.
3. Isolated failures after a client change may be a client incorrectly sending
   `Origin`; a browser burst against login/write routes is CSRF probing. Contain
   a clear hostile source at Cloudflare and keep the application guard enabled.
4. Duplicate, opaque and malformed origins are deliberately indistinguishable
   in public responses and telemetry; do not weaken that behavior for debugging.

## Request contract rejections

1. Confirm the bounded `unsupported_media_type/blocked` series and split the
   affected route shapes using redacted request logs and IDs.
2. After a release, first rule out an old or third-party client sending API JSON
   as `text/plain`, or sending login credentials with a JSON media type. Correct
   that client; do not broaden parsers to guess payload formats.
3. Never add raw `Content-Type` values to logs or metrics. A sustained hostile
   pattern is input probing and can be contained at the edge using route and
   source signals already available there.

## Idempotency-key misuse

1. Split `idempotency_rejected/blocked` from `idempotency_conflict/blocked` and
   correlate only request IDs, route shapes and actor classes. Never recover or
   log the supplied key or request fingerprint.
2. Rejections usually mean a client emitted malformed or duplicate headers;
   conflicts mean it reused one token for different request material. Correct
   retry middleware to retain a token only for retries of the exact operation.
3. A distributed or sustained unexplained burst is protocol probing. Contain a
   clear source pattern at Cloudflare while keeping CSRF, rate limits and the
   conflict response enabled; idempotency is not an authentication boundary.

## Share-link probing or expiry noise

1. Check whether an owner just rotated, disabled or allowed a link to expire;
   those normal events can produce a short burst of blocked views.
2. If unexplained, correlate only redacted `/d/<redacted>` request records by
   request ID, time window and edge metadata. Never recover or print the token.
3. A sustained distributed pattern is capability probing: contain it at
   Cloudflare/nginx, preserve aggregate evidence and follow the incident runbook
   if any allowed views appear in the same window.

## Share-password attack

1. Confirm `NuiGraphSharePasswordAttack` is driven by aggregate `failed` or
   `rate_limited` outcomes. Correlate only redacted `/d/<redacted>/unlock`
   records, request IDs, time windows and approved edge metadata; never recover
   or print a password, verifier or share slug.
2. Distinguish a legitimate recipient mistyping a recently changed password
   from distributed guessing. A successful unlock in the same window may mean
   the link and password were compromised; preserve evidence and follow
   `docs/INCIDENT_RESPONSE.md`.
3. Contain a clear hostile source pattern at Cloudflare/nginx without disabling
   the database-backed limiter. The owner can replace the password, rotate the
   link or disable sharing; each option revokes existing unlock cookies.
4. An unlock cookie is valid for at most eight hours and only for its bound
   link/password version. Do not rely on expiry when compromise is plausible:
   use an explicit revoking transition and verify protected page, API and fork
   routes all reject the old cookie.

## Destructive trash activity

1. Treat ten permanent deletions in ten minutes as potentially destructive even
   if capacity is high. Correlate bounded transition counts, actor classes and
   request IDs; do not add diagram IDs, titles or owner identities to metrics.
2. Determine whether the window is an expected owner cleanup or an unexplained
   admin/guest session takeover. Preserve the journal and edge evidence before
   changing keys or restarting services.
3. For unexplained activity, use the incident runbook's emergency maintenance
   deny to stop further mutations, rotate a compromised signing key without a
   previous-key grace window, and assess the full database blast radius.
4. Permanent deletion removes primary rows and versions. Do not promise recovery
   or GDPR erasure from other stores without verified backup/log retention and a
   tested restore or deletion procedure.

## Automatic trash retention

1. Treat `NuiGraphTrashPurgeFailure`, `NuiGraphTrashPurgeStalled` or
   `NuiGraphTrashAutoPurgeSpike` as destructive-data incidents until explained.
   Preserve Prometheus and journal evidence; maintenance logs deliberately carry
   only aggregate deleted counts and the configured age, never IDs or titles.
2. Contain by setting `TRASH_RETENTION_DAYS=0` and
   `TRASH_RETENTION_CONFIRM=false`, validating the candidate configuration and
   restarting the service. If the actor or host is suspect, use the emergency
   maintenance deny before editing configuration. Do not modify `deleted_at` or
   the coordination row to simulate recovery.
3. Compare `nuigraph_trash_purged_total`, the backlog and logical-byte change
   with `maintenance_jobs.last_success_at` and `last_deleted_count`. One claimed
   run per configured interval is expected globally, even with multiple app
   instances; additional removals or a changed retention age require escalation.
4. Determine whether affected rows were older than the approved policy and
   whether the two-key activation was reviewed. The database clock, oldest-first
   ordering and bounded batch reduce race/blast radius but cannot recover an
   already committed cascade delete.
5. Recovery is unavailable while verified backups are deferred. Record that
   residual explicitly in the incident, assess notification obligations, and do
   not re-enable automatic retention until paging and recovery assumptions have
   been re-approved.

## Privacy data activity

1. Treat `NuiGraphOwnerDataDeleted` as a destructive-data event requiring
   prompt review. A legitimate guest cleanup is possible, but the aggregate
   signal cannot establish intent or identity. Preserve the related request ID,
   actor class and edge evidence without adding an owner hash, diagram ID/title,
   confirmation phrase or exported content to a ticket.
2. For `NuiGraphOwnerDataExportSpike`, compare the window with total request,
   write-throttle and blocked privacy-control events. A single user retrying a
   download, automated probing and multiple legitimate sessions look similar in
   bounded metrics; use only already-approved redacted edge metadata to decide.
3. Unexplained export/deletion activity can indicate a stolen guest cookie.
   Use the incident runbook's maintenance deny if activity continues, rotate a
   compromised signing key without the previous-key grace window, and assess
   the owner-session blast radius. Do not enable an administrator bulk endpoint
   for investigation.
4. Primary-row deletion is irreversible in the application. It does not prove
   erasure from external logs or backup media. Recovery and offsite deletion
   remain unavailable/unverified while the deferred backup and retention work
   is open; record that limitation rather than promising recovery or compliance.

## Unexpected restart

Confirm whether a deploy explains the start-time change. Otherwise preserve the
preceding journal, inspect systemd exit status/OOM evidence and open an incident
for repeated or unexplained restarts.

## Certificate expiry

Check Certbot timer state and the ACME challenge path through the public domain.
Use `sudo nginx -t` before a reload; never restart the shared nginx service.

## Host disk pressure

Identify growth by filesystem and service before deleting anything. Prefer
retention controls and verified disposable build/cache data. Database files,
release directories, logs and any future backup artifacts require separate
ownership and recovery checks before cleanup.
