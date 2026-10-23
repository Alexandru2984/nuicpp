#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "global quota test"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18451 18550)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
PID=""
TEST_TITLE_PREFIX="ngs-global-quota-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"
CLIENTS=6

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "${TEST_TITLE_PREFIX:-}" ]]; then
    psql --dbname="$DATABASE_URL" -X -qtAc \
      "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# Count only after removing leftovers from an interrupted earlier run. Setting
# the cap to baseline + 1 makes the assertion independent of other test rows.
psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null
BASELINE="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "SELECT count(*) FROM diagrams")"
BASELINE="${BASELINE//[[:space:]]/}"
GLOBAL_CAP=$((BASELINE + 1))

cat > "$TMP_ENV" <<ENV_EOF
APP_ENV=test
APP_HOST=127.0.0.1
APP_PORT=$PORT
APP_BASE_URL=http://127.0.0.1:$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}
SESSION_SIGNING_SECRET=${TEST_SIGNING_SECRET}
CSRF_SECRET=${TEST_CSRF_SECRET}
OWNER_HASH_SECRET=${TEST_OWNER_SECRET}
RATE_LIMIT_SECRET=${TEST_RATE_LIMIT_SECRET}
PUBLIC_ACCESS=true
MAX_DIAGRAMS_PER_GUEST=100
MAX_DIAGRAMS_TOTAL=$GLOBAL_CAP
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
    echo "global quota test failed: server crashed during startup:" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ "$healthy" != "1" ]]; then
  echo "global quota test failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

# Prepare distinct guest identities before releasing their create requests at
# once. Cookie rotation therefore cannot bypass the single remaining slot.
request_pids=()
for i in $(seq 1 "$CLIENTS"); do
  cookie_jar="$TMP_DIR/cookies-$i.txt"
  session_resp="$(curl --noproxy '*' -fsS -c "$cookie_jar" "${BASE}/api/session")"
  python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" \
    "$session_resp" >"$TMP_DIR/csrf-$i.txt"
done

for i in $(seq 1 "$CLIENTS"); do
  (
    csrf_token="$(<"$TMP_DIR/csrf-$i.txt")"
    payload="{\"title\":\"${TEST_TITLE_PREFIX}${i}\",\"description\":\"\",\"nodes\":[],\"edges\":[]}"
    curl --noproxy '*' -sS -o "$TMP_DIR/body-$i.json" -w '%{http_code}' \
      -b "$TMP_DIR/cookies-$i.txt" \
      -H "X-CSRF-Token: $csrf_token" \
      -H "Content-Type: application/json" \
      -X POST -d "$payload" "${BASE}/api/diagrams" >"$TMP_DIR/status-$i.txt"
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
      python3 -c "import sys,json; assert 'capacity' in json.load(open(sys.argv[1]))['error']" \
        "$TMP_DIR/body-$i.json"
      ;;
    *)
      echo "global quota test failed: client $i returned HTTP $status" >&2
      cat "$TMP_DIR/body-$i.json" >&2
      exit 1
      ;;
  esac
done

stored="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "SELECT count(*) FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'")"
stored="${stored//[[:space:]]/}"

if [[ "$created" != "1" || "$capacity_rejected" != "$((CLIENTS - 1))" || "$stored" != "1" ]]; then
  echo "global quota test failed: created=$created rejected=$capacity_rejected stored=$stored" >&2
  exit 1
fi

echo "global quota test passed"
