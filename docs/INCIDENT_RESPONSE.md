# Incident response runbook

This runbook covers suspected diagram disclosure, admin/session compromise,
storage abuse, production outage and supply-chain compromise. The incident lead
is the VPS operator until another named owner takes over in the incident record.

## Severity

- **SEV-1:** confirmed admin/session-key or host compromise, bulk diagram access,
  destructive database event, active supply-chain compromise, or unrecoverable
  outage.
- **SEV-2:** suspected limited disclosure, sustained abuse, repeated 5xx/data-path
  failure, backup failure with no current recovery point.
- **SEV-3:** blocked attack, isolated defect without data loss, or degraded
  non-critical feature.

## First 15 minutes

1. Open a timestamped incident record. Record reporter, symptoms, first-known and
   last-known-good times, affected domain/release, and every command or decision.
2. Preserve evidence before rotating or deleting: relevant journal/nginx slices,
   release ID and hashes, process metadata, database audit metadata, Cloudflare
   events and authentication events. Do not copy diagram bodies unless necessary.
3. Contain with the smallest reversible control:
   - disable public writes at Cloudflare/nginx or set `PUBLIC_ACCESS=false`;
   - block an abusive source at the edge;
   - switch `current` to a known-good root-owned release;
   - for confirmed host compromise, remove the origin from service and isolate it.
4. Confirm whether admin credentials, `SESSION_SECRET`, database credentials,
   bearer share links or backup credentials may have been read. Treat readable
   secrets as compromised, not merely suspicious.
5. Assign an incident lead and a communications owner. For SEV-1, start a
   15-minute update cadence until contained.

## Investigation checklist

- Compare the running binary and public assets with the expected release hashes.
- Inspect systemd restarts, error/5xx rates, unexpected processes/listeners,
  authentication attempts and accepted non-Cloudflare peers.
- Review database role attributes, grants, active sessions, row-count/size changes
  and destructive statements where audit evidence exists. Avoid selecting user
  content into terminals or tickets.
- Review GitHub changes, dependency updates, deploy history, npm/CMake inputs and
  the integrity of the checked-out commit.
- Scope affected diagrams, versions and time range. Identify whether content may
  contain personal data, credentials or third-party confidential information.

## Eradication and recovery

1. Patch or roll back the entry point before restoring public traffic.
2. Rotate credentials in dependency order: database and infrastructure tokens,
   admin password, then session-signing material. Current `SESSION_SECRET`
   rotation orphans guest-owned diagrams; preserve the old key offline if a
   controlled ownership migration is required, never keep both active casually.
3. Restore only from a verified backup into an isolated database, validate schema
   and counts, and test read/write/version flows before cutover.
4. Run build, unit, isolated integration, security smoke, rate-limit, migration,
   nginx-config and external health checks. Do not run destructive tests against
   production data.
5. Monitor at heightened sensitivity for at least 24 hours after recovery.

## Communication and regulation

For suspected personal-data exposure, record when the controller became aware,
categories and approximate number of people/records, likely consequences and
mitigations. Assess GDPR supervisory-authority notification immediately; where
required, the deadline is 72 hours from awareness. If notification is delayed,
document the reason. Notify affected people without undue delay when the breach
is likely to create high risk. Obtain legal advice for the actual jurisdiction
and facts.

Do not publish raw logs, session cookies, share slugs, IP lists, diagram titles or
diagram bodies in status updates. Use stable incident IDs and aggregate counts.

## Closure

- Verify recovery objectives with a real restore, not only a successful backup.
- Write a blameless timeline, root cause, blast radius and control gaps.
- Give every action an owner and due date; track high-risk actions to completion.
- Update threat model, tests, alerts and this runbook.
- Run a tabletop at least twice yearly and after any SEV-1 or material design
  change. Record the date and outcomes here or in the operations system.

**Last tabletop:** not yet performed.

