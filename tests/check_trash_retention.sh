#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "trash retention"

PORT_ONE="$("$ROOT_DIR/scripts/find_free_port.sh" 19051 19150)"
PORT_TWO="$("$ROOT_DIR/scripts/find_free_port.sh" "$((PORT_ONE + 1))" "$((PORT_ONE + 100))")"
TMP_DIR="$(mktemp -d)"
ENV_ONE="$TMP_DIR/one.env"
ENV_TWO="$TMP_DIR/two.env"
LOG_ONE="$TMP_DIR/one.log"
LOG_TWO="$TMP_DIR/two.log"
PID_ONE=""
PID_TWO=""
TEST_PREFIX="ngs-retention-${PORT_ONE}-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"

cleanup() {
  for pid in "$PID_ONE" "$PID_TWO"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
      wait "$pid" >/dev/null 2>&1 || true
    fi
  done
  psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_PREFIX}%';
     UPDATE maintenance_jobs
     SET next_run_at='-infinity', last_success_at=NULL, last_deleted_count=0
     WHERE job_name='trash_purge';" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

BASELINE_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"

read -r OLD_ONE OLD_TWO OLD_THREE FRESH ACTIVE < <(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -AtF ' ' <<SQL
WITH inserted AS (
  INSERT INTO diagrams
    (title, slug, description, sharing_enabled, deleted_at, logical_bytes)
  VALUES
    ('${TEST_PREFIX}old-one', '${TEST_PREFIX}old-one', '', false, statement_timestamp() - interval '11 days', 101),
    ('${TEST_PREFIX}old-two', '${TEST_PREFIX}old-two', '', false, statement_timestamp() - interval '10 days', 102),
    ('${TEST_PREFIX}old-three', '${TEST_PREFIX}old-three', '', false, statement_timestamp() - interval '9 days', 103),
    ('${TEST_PREFIX}fresh', '${TEST_PREFIX}fresh', '', false, statement_timestamp() - interval '6 days', 104),
    ('${TEST_PREFIX}active', '${TEST_PREFIX}active', '', false, NULL, 105)
  RETURNING id, title
)
SELECT
  max(id) FILTER (WHERE title='${TEST_PREFIX}old-one'),
  max(id) FILTER (WHERE title='${TEST_PREFIX}old-two'),
  max(id) FILTER (WHERE title='${TEST_PREFIX}old-three'),
  max(id) FILTER (WHERE title='${TEST_PREFIX}fresh'),
  max(id) FILTER (WHERE title='${TEST_PREFIX}active')
FROM inserted;
SQL
)

psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -q <<SQL
INSERT INTO nodes
  (diagram_id, node_key, type, title, x, y, width, height, color, metadata_json)
VALUES (${OLD_ONE}, 'retained_node', 'note', 'retained', 0, 0, 100, 80, '#38bdf8', '{}'::jsonb);
INSERT INTO diagram_versions
  (diagram_id, version_number, snapshot_json, note, logical_bytes)
VALUES (${OLD_ONE}, 1, '{"title":"retained"}'::jsonb, 'retained', 201);
INSERT INTO diagram_idempotency_keys
  (principal_key, operation, key_hash, request_hash, diagram_id)
VALUES (repeat('a', 64), 'create', repeat('b', 64), repeat('c', 64), ${OLD_ONE});
UPDATE maintenance_jobs
SET next_run_at='-infinity', last_success_at=NULL, last_deleted_count=0
WHERE job_name='trash_purge';
SQL

write_env() {
  local path="$1"
  local port="$2"
  cat >"$path" <<ENV_EOF
APP_ENV=test
APP_HOST=127.0.0.1
APP_PORT=$port
APP_BASE_URL=http://127.0.0.1:$port
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}
SESSION_SIGNING_SECRET=${TEST_SIGNING_SECRET}
CSRF_SECRET=${TEST_CSRF_SECRET}
OWNER_HASH_SECRET=${TEST_OWNER_SECRET}
RATE_LIMIT_SECRET=${TEST_RATE_LIMIT_SECRET}
PUBLIC_ACCESS=true
TRASH_RETENTION_DAYS=7
TRASH_RETENTION_CONFIRM=true
TRASH_PURGE_INTERVAL_SECONDS=60
TRASH_PURGE_BATCH_SIZE=2
DB_POOL_SIZE=2
PROJECT_ROOT=$ROOT_DIR
ENV_EOF
}
write_env "$ENV_ONE" "$PORT_ONE"
write_env "$ENV_TWO" "$PORT_TWO"

env -i PATH="$PATH" "$BINARY" "$ENV_ONE" >"$LOG_ONE" 2>&1 &
PID_ONE="$!"
env -i PATH="$PATH" "$BINARY" "$ENV_TWO" >"$LOG_TWO" 2>&1 &
PID_TWO="$!"

for entry in "$PORT_ONE:$PID_ONE" "$PORT_TWO:$PID_TWO"; do
  port="${entry%%:*}"
  pid="${entry##*:}"
  healthy=0
  for _ in $(seq 1 150); do
    if curl --noproxy '*' -fsS "http://127.0.0.1:${port}/ready" >/dev/null 2>&1; then
      healthy=1
      break
    fi
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      echo "trash retention failed: server on port $port exited" >&2
      cat "$LOG_ONE" "$LOG_TWO" >&2
      exit 1
    fi
    sleep 0.1
  done
  [[ "$healthy" == "1" ]] || { echo "trash retention failed: server on port $port was not ready" >&2; exit 1; }
done

purged=0
for _ in $(seq 1 100); do
  purged="$(psql --dbname="$DATABASE_URL" -X -Atqc \
    "SELECT count(*) FROM diagrams WHERE id IN (${OLD_ONE}, ${OLD_TWO})")"
  [[ "$purged" == "0" ]] && break
  sleep 0.05
done
[[ "$purged" == "0" ]] || { echo "trash retention failed: expired batch was not purged" >&2; exit 1; }

remaining="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT string_agg(id::text, ',' ORDER BY id) FROM diagrams
   WHERE id IN (${OLD_THREE}, ${FRESH}, ${ACTIVE})")"
expected_remaining="$(printf '%s\n' "$OLD_THREE" "$FRESH" "$ACTIVE" | sort -n | paste -sd, -)"
[[ "$remaining" == "$expected_remaining" ]] || {
  echo "trash retention failed: batch limit or age boundary was not preserved" >&2
  exit 1
}

cascade_count="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT (SELECT count(*) FROM nodes WHERE diagram_id=${OLD_ONE})
        + (SELECT count(*) FROM diagram_versions WHERE diagram_id=${OLD_ONE})
        + (SELECT count(*) FROM diagram_idempotency_keys WHERE diagram_id=${OLD_ONE})")"
[[ "$cascade_count" == "0" ]] || { echo "trash retention failed: dependent rows survived purge" >&2; exit 1; }

FINAL_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"
[[ "$FINAL_BYTES" == "$((BASELINE_BYTES + 312))" ]] || {
  echo "trash retention failed: logical quota did not release the purged batch" >&2
  exit 1
}

job_state="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT (last_success_at IS NOT NULL)::int || ':' || last_deleted_count || ':' || (next_run_at > statement_timestamp())::int
   FROM maintenance_jobs WHERE job_name='trash_purge'")"
[[ "$job_state" == "1:2:1" ]] || { echo "trash retention failed: global schedule state is $job_state" >&2; exit 1; }

METRICS_ONE="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT_ONE}/metrics")"
METRICS_TWO="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT_TWO}/metrics")"
for metrics in "$METRICS_ONE" "$METRICS_TWO"; do
  grep -Fqx 'nuigraph_trash_retention_days 7' <<<"$metrics"
  grep -Fqx 'nuigraph_trash_purge_interval_seconds 60' <<<"$metrics"
  grep -Eq '^nuigraph_trash_purge_last_success_timestamp_seconds [1-9][0-9]*$' <<<"$metrics"
done
metric_sum() {
  local metric="$1"
  printf '%s\n%s\n' "$METRICS_ONE" "$METRICS_TWO" |
    awk -v name="$metric" '$1 == name { total += $2 } END { print total + 0 }'
}
[[ "$(metric_sum 'nuigraph_trash_purged_total')" == "2" ]]
[[ "$(metric_sum 'nuigraph_trash_purge_runs_total{outcome="success"}')" == "1" ]]
[[ "$(metric_sum 'nuigraph_trash_purge_runs_total{outcome="error"}')" == "0" ]]

for port in "$PORT_ONE" "$PORT_TWO"; do
  policy="$(curl --noproxy '*' -fsS "http://127.0.0.1:${port}/api/trash")"
  python3 -c 'import json,sys; assert json.loads(sys.argv[1])["retention_days"] == 7' "$policy"
done

cat "$LOG_ONE" "$LOG_TWO" >"$TMP_DIR/combined.log"
[[ "$(grep -c 'category=maintenance event=trash_purge outcome=succeeded deleted=2 retention_days=7' "$TMP_DIR/combined.log")" == "1" ]]
if grep -Fq "$TEST_PREFIX" "$TMP_DIR/combined.log"; then
  echo "trash retention failed: maintenance log disclosed diagram metadata" >&2
  exit 1
fi

echo "trash retention passed"
