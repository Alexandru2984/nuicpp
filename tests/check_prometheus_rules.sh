#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RULES="$ROOT_DIR/deploy/monitoring/nuigraph-alerts.yml"
PINNED_PROMETHEUS="prom/prometheus@sha256:2659f4c2ebb718e7695cb9b25ffa7d6be64db013daba13e05c875451cf51b0d3"

for alert in \
  NuiGraphTargetDown \
  NuiGraphDatabaseDown \
  NuiGraphReadinessFailed \
  NuiGraphHighFiveXxRate \
  NuiGraphHighP95Latency \
  NuiGraphDatabasePoolTimeout \
  NuiGraphCreationSpike \
  NuiGraphAdminLoginAttack \
  NuiGraphAdminLoginSucceeded \
  NuiGraphCrossSiteMutationSpike \
  NuiGraphBlockedShareViewSpike \
  NuiGraphRateLimitStorageFailure \
  NuiGraphDiagramCapacityHigh \
  NuiGraphLogicalStorageCapacityHigh \
  NuiGraphLimiterCapacityHigh \
  NuiGraphDatabaseGrowthHigh \
  NuiGraphUnexpectedRestart \
  NuiGraphCertificateExpiresSoon \
  NuiGraphHostDiskLow
do
  if ! grep -Fq "alert: $alert" "$RULES"; then
    echo "Prometheus rules are missing $alert" >&2
    exit 1
  fi
done

if command -v promtool >/dev/null 2>&1; then
  promtool check rules "$RULES"
elif command -v docker >/dev/null 2>&1; then
  docker run --rm --entrypoint /bin/promtool \
    -v "$ROOT_DIR:/workspace:ro" "$PINNED_PROMETHEUS" \
    check rules /workspace/deploy/monitoring/nuigraph-alerts.yml
elif [[ "${NUIGRAPH_REQUIRE_LIVE_TESTS:-0}" == "1" ]]; then
  echo "Prometheus rule validation failed: promtool and Docker are unavailable" >&2
  exit 1
else
  echo "Prometheus rule validation skipped: promtool and Docker are unavailable"
  exit 77
fi

echo "Prometheus alert rules passed"
