#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNBOOK="$ROOT_DIR/docs/INCIDENT_RESPONSE.md"
EXERCISE="$ROOT_DIR/docs/INCIDENT_TABLETOP_2026-10-23.md"

if [[ ! -f "$EXERCISE" ]]; then
  echo "incident tabletop record is missing" >&2
  exit 1
fi

required_record_text=(
  'NGS-TTX-2026-01'
  'production systems'
  'were not changed'
  'SEV-1'
  'request_id'
  'PUBLIC_ACCESS=false'
  'SESSION_SECRET'
  'SESSION_SIGNING_SECRET_PREVIOUS'
  'tests/check_key_rotation.sh'
  '72'
  'hours after awareness'
  '2026-10-26 10:18 EEST'
  'Conditional pass'
  'IR-001'
  'IR-006'
  '2027-04-23'
)
for text in "${required_record_text[@]}"; do
  if ! grep -Fq "$text" "$EXERCISE"; then
    echo "incident tabletop is missing required evidence: $text" >&2
    exit 1
  fi
done

required_sections=(
  '## Objectives and guardrails'
  '## Scenario, timeline and decisions'
  '## Evidence checklist'
  '## Communications and GDPR decision'
  '## Recovery criteria'
  '## Findings and action register'
  '## Exercise result'
)
for section in "${required_sections[@]}"; do
  if ! grep -Fxq "$section" "$EXERCISE"; then
    echo "incident tabletop is missing section: $section" >&2
    exit 1
  fi
done

if ! grep -Fq '[2026-10-23 — NGS-TTX-2026-01](INCIDENT_TABLETOP_2026-10-23.md)' "$RUNBOOK"; then
  echo "incident runbook does not link to the latest exercise" >&2
  exit 1
fi
if grep -Eqi 'last tabletop:.*(not yet|tbd|todo)' "$RUNBOOK"; then
  echo "incident runbook still reports an unperformed tabletop" >&2
  exit 1
fi

action_count="$(grep -Ec '^\| IR-[0-9]{3} \|' "$EXERCISE")"
if [[ "$action_count" -lt 1 ]]; then
  echo "incident tabletop has no assigned follow-up actions" >&2
  exit 1
fi
if grep -E '^\| IR-[0-9]{3} \|' "$EXERCISE" \
    | grep -Ev '\| (VPS operator|Data controller|Maintainer) \| 20[0-9]{2}-[01][0-9]-[0-3][0-9] \|' >/dev/null; then
  echo "incident action is missing a recognised owner or ISO due date" >&2
  exit 1
fi

echo "incident-response documentation passed"
