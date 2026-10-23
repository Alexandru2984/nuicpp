#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "logical storage quota test"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18551 18650)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
PID=""
TEST_TITLE_PREFIX="ngs-logical-quota-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SESSION_SECRET="$(openssl rand -hex 32)"
CLIENTS=6
OWNER_CAP=8000
GLOBAL_HEADROOM=12000

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  psql --dbname="$DATABASE_URL" -X -qtAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null
BASELINE="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true")"
BASELINE="${BASELINE//[[:space:]]/}"
GLOBAL_CAP=$((BASELINE + GLOBAL_HEADROOM))

cat > "$TMP_ENV" <<ENV_EOF
APP_ENV=test
APP_HOST=127.0.0.1
APP_PORT=$PORT
APP_BASE_URL=http://127.0.0.1:$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}
SESSION_SECRET=${TEST_SESSION_SECRET}
PUBLIC_ACCESS=true
MAX_DIAGRAMS_PER_GUEST=100
MAX_DIAGRAMS_TOTAL=1000000
MAX_LOGICAL_BYTES_PER_GUEST=$OWNER_CAP
MAX_LOGICAL_BYTES_TOTAL=$GLOBAL_CAP
PROJECT_ROOT=$ROOT_DIR
ENV_EOF

env -i PATH="$PATH" "$BINARY" "$TMP_ENV" >"$LOG_FILE" 2>&1 &
PID="$!"
BASE="http://127.0.0.1:${PORT}"

healthy=0
for _ in $(seq 1 150); do
  if curl --noproxy '*' -fsS "${BASE}/ready" >/dev/null 2>&1; then
    healthy=1
    break
  fi
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "logical quota test failed: server crashed during startup:" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
if [[ "$healthy" != "1" ]]; then
  echo "logical quota test failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

# An oversized update must roll back its revision, nodes, snapshot and counter.
COOKIE_JAR="$TMP_DIR/owner.cookies"
session_json="$(curl --noproxy '*' -fsS -c "$COOKIE_JAR" "${BASE}/api/session")"
CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$session_json")"
CREATE_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}rollback\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
DIAGRAM_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$CREATE_RESP")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$CREATE_RESP")"
BLOB="$(printf '%*s' 5000 '' | tr ' ' x)"
status="$(curl --noproxy '*' -sS -o "$TMP_DIR/owner-limit.json" -w '%{http_code}' \
  -b "$COOKIE_JAR" -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" -X PUT \
  -d "{\"title\":\"${TEST_TITLE_PREFIX}rollback\",\"description\":\"\",\"nodes\":[{\"key\":\"n1\",\"type\":\"note\",\"title\":\"large\",\"x\":0,\"y\":0,\"width\":160,\"height\":80,\"color\":\"#38bdf8\",\"metadata\":{\"blob\":\"$BLOB\"}}],\"edges\":[]}" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}")"
if [[ "$status" != "403" ]]; then
  echo "logical quota test failed: oversized owner update returned $status" >&2
  cat "$TMP_DIR/owner-limit.json" >&2
  exit 1
fi
python3 -c "import json,sys; assert 'logical storage limit' in json.load(open(sys.argv[1]))['error']" \
  "$TMP_DIR/owner-limit.json"
UNCHANGED="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "${BASE}/api/diagrams/${DIAGRAM_ID}")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["revision"]==int(sys.argv[2]); assert d["nodes"]==[]' \
  "$UNCHANGED" "$REVISION"

curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${DIAGRAM_ID}" >/dev/null
AFTER_DELETE="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true")"
AFTER_DELETE="${AFTER_DELETE//[[:space:]]/}"
if [[ "$AFTER_DELETE" != "$BASELINE" ]]; then
  echo "logical quota test failed: delete did not restore the logical-byte counter" >&2
  exit 1
fi

# Distinct sessions race for capacity. The transactional singleton permits one
# payload and rolls every other insert and initial snapshot back atomically.
request_pids=()
for i in $(seq 1 "$CLIENTS"); do
  cookie_jar="$TMP_DIR/cookies-$i.txt"
  session_resp="$(curl --noproxy '*' -fsS -c "$cookie_jar" "${BASE}/api/session")"
  python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' \
    "$session_resp" >"$TMP_DIR/csrf-$i.txt"
done
CONCURRENT_BLOB="$(printf '%*s' 3000 '' | tr ' ' q)"
for i in $(seq 1 "$CLIENTS"); do
  (
    csrf_token="$(<"$TMP_DIR/csrf-$i.txt")"
    curl --noproxy '*' -sS -o "$TMP_DIR/body-$i.json" -w '%{http_code}' \
      -b "$TMP_DIR/cookies-$i.txt" -H "X-CSRF-Token: $csrf_token" \
      -H 'Content-Type: application/json' -X POST \
      -d "{\"title\":\"${TEST_TITLE_PREFIX}${i}\",\"description\":\"\",\"nodes\":[{\"key\":\"n1\",\"type\":\"note\",\"title\":\"capacity\",\"x\":0,\"y\":0,\"width\":160,\"height\":80,\"color\":\"#38bdf8\",\"metadata\":{\"blob\":\"$CONCURRENT_BLOB\"}}],\"edges\":[]}" \
      "${BASE}/api/diagrams" >"$TMP_DIR/status-$i.txt"
  ) &
  request_pids+=("$!")
done
for request_pid in "${request_pids[@]}"; do
  wait "$request_pid"
done

created=0
capacity_rejected=0
for i in $(seq 1 "$CLIENTS"); do
  status="$(<"$TMP_DIR/status-$i.txt")"
  case "$status" in
    201) created=$((created + 1)) ;;
    503)
      capacity_rejected=$((capacity_rejected + 1))
      python3 -c "import json,sys; assert 'logical storage capacity' in json.load(open(sys.argv[1]))['error']" \
        "$TMP_DIR/body-$i.json"
      ;;
    *)
      echo "logical quota test failed: concurrent client $i returned HTTP $status" >&2
      cat "$TMP_DIR/body-$i.json" >&2
      exit 1
      ;;
  esac
done
STORED="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "SELECT count(*) FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'")"
STORED="${STORED//[[:space:]]/}"
if [[ "$created" != "1" || "$capacity_rejected" != "$((CLIENTS - 1))" || "$STORED" != "1" ]]; then
  echo "logical quota test failed: created=$created rejected=$capacity_rejected stored=$STORED" >&2
  exit 1
fi

COUNTERS="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -AtF ' ' -c \
  "SELECT u.logical_bytes,
          (SELECT COALESCE(sum(logical_bytes),0) FROM diagrams)
          + (SELECT COALESCE(sum(logical_bytes),0) FROM diagram_versions)
   FROM logical_storage_usage u WHERE singleton=true")"
read -r TRACKED RECOMPUTED <<<"$COUNTERS"
if [[ "$TRACKED" != "$RECOMPUTED" || "$TRACKED" -gt "$GLOBAL_CAP" ]]; then
  echo "logical quota test failed: tracked=$TRACKED recomputed=$RECOMPUTED cap=$GLOBAL_CAP" >&2
  exit 1
fi

METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
grep -Fqx "nuigraph_logical_storage_bytes $TRACKED" <<<"$METRICS"
grep -Fqx "nuigraph_logical_storage_capacity_bytes $GLOBAL_CAP" <<<"$METRICS"
grep -q 'category=security event=owner_quota_exhausted outcome=blocked' "$LOG_FILE"
grep -q 'category=security event=global_capacity_exhausted outcome=blocked' "$LOG_FILE"

echo "logical storage quota test passed"
