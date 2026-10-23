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

The bounded security series includes allowed/blocked share views and successful
enable/disable/rotate/expiry-update lifecycle changes. Runtime logs use the same outcomes but
redact the capability from both page and API paths. A sudden blocked-view spike
after a planned rotation is expected; an unexplained sustained spike should be
correlated with access logs without recovering or printing the old token.

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
signals intentionally require longer persistence to limit noise.

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
   capacity and physical DB byte growth over the same window. Logical bytes
   cover retained user content; DB bytes also include indexes, bloat and system
   overhead.
2. Contain at Cloudflare/nginx first when the source pattern is clear. Do not
   delete rate-limit rows or user diagrams merely to silence an alert.
3. Preserve aggregate evidence and open an incident when global capacity can be
   exhausted or legitimate creation is being denied.
4. Do not raise a logical ceiling until disk headroom and recent growth are
   understood. Prefer pruning or an explicit retention decision; deleting a
   diagram transactionally releases its current and historical byte count.

## Authentication attack

1. Correlate the alert with redacted `admin_login` events and Cloudflare data.
2. Block abusive sources at the edge; do not weaken application throttling.
3. Treat any unexplained successful login near the attack window as a potential
   credential incident and follow `docs/INCIDENT_RESPONSE.md`.

## Share-link probing or expiry noise

1. Check whether an owner just rotated, disabled or allowed a link to expire;
   those normal events can produce a short burst of blocked views.
2. If unexplained, correlate only redacted `/d/<redacted>` request records by
   request ID, time window and edge metadata. Never recover or print the token.
3. A sustained distributed pattern is capability probing: contain it at
   Cloudflare/nginx, preserve aggregate evidence and follow the incident runbook
   if any allowed views appear in the same window.

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
