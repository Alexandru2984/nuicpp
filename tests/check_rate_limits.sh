#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "rate limits test"

PORT_A="$("$ROOT_DIR/scripts/find_free_port.sh" 18351 18400)"
PORT_B="$("$ROOT_DIR/scripts/find_free_port.sh" "$((PORT_A + 1))" 18450)"
TMP_DIR="$(mktemp -d)"
ENV_A="$TMP_DIR/app-a.env"
ENV_B="$TMP_DIR/app-b.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
LOG_A="$TMP_DIR/server-a.log"
LOG_B="$TMP_DIR/server-b.log"
TEST_TITLE_PREFIX="ngs-ratetest-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"
PIDS=()

stop_instances() {
  local pid
  for pid in "${PIDS[@]}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
    fi
  done
  for pid in "${PIDS[@]}"; do
    wait "$pid" >/dev/null 2>&1 || true
  done
  PIDS=()
}

cleanup() {
  stop_instances
  # The helper has already proved this is a dedicated *_test database and role.
  # Clear shared limiter state as well as diagrams so other live suites start
  # independently even when this test exits on a failed assertion.
  psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -c \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'; DELETE FROM rate_limit_buckets;" \
    >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

write_env() {
  local path="$1"
  local port="$2"
  {
    printf '%s\n' \
      'APP_ENV=test' \
      'APP_HOST=127.0.0.1' \
      "APP_PORT=$port" \
      "APP_BASE_URL=http://127.0.0.1:$port" \
      "DATABASE_URL=$DATABASE_URL" \
      'AUTH_USERNAME=admin' \
      "AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}" \
      "SESSION_SIGNING_SECRET=${TEST_SIGNING_SECRET}" \
      "CSRF_SECRET=${TEST_CSRF_SECRET}" \
      "OWNER_HASH_SECRET=${TEST_OWNER_SECRET}" \
      "RATE_LIMIT_SECRET=${TEST_RATE_LIMIT_SECRET}" \
      'PUBLIC_ACCESS=true' \
      'MAX_DIAGRAMS_PER_GUEST=60' \
      "PROJECT_ROOT=$ROOT_DIR"
  } > "$path"
}

start_instance() {
  local env_file="$1"
  local port="$2"
  local log_file="$3"
  local pid
  local healthy=0

  env -i PATH="$PATH" "$BINARY" "$env_file" >"$log_file" 2>&1 &
  pid="$!"
  PIDS+=("$pid")

  for _ in $(seq 1 150); do
    if curl --noproxy '*' -fsS "http://127.0.0.1:${port}/ready" >/dev/null 2>&1; then
      healthy=1
      break
    fi
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      echo "rate limits test failed: server on port $port crashed during startup" >&2
      cat "$log_file" >&2
      exit 1
    fi
    sleep 0.1
  done

  if [[ "$healthy" != "1" ]]; then
    echo "rate limits test failed: server on port $port did not become ready" >&2
    cat "$log_file" >&2
    exit 1
  fi
}

write_env "$ENV_A" "$PORT_A"
write_env "$ENV_B" "$PORT_B"
start_instance "$ENV_A" "$PORT_A" "$LOG_A"
start_instance "$ENV_B" "$PORT_B" "$LOG_B"

BASE_A="http://127.0.0.1:${PORT_A}"
BASE_B="http://127.0.0.1:${PORT_B}"

# Concurrent requests split across independent processes must still consume one
# atomic login bucket. Exactly eight attempts reach credential verification; the
# other eight are rejected before PBKDF2 work is performed.
LOGIN_PIDS=()
for i in $(seq 1 16); do
  if (( i % 2 == 0 )); then
    base="$BASE_B"
  else
    base="$BASE_A"
  fi
  curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data 'username=invalid&password=invalid' "$base/login" \
    >"$TMP_DIR/login-$i.status" &
  LOGIN_PIDS+=("$!")
done
for pid in "${LOGIN_PIDS[@]}"; do
  wait "$pid"
done

login_unauthorized=0
login_limited=0
for i in $(seq 1 16); do
  status="$(<"$TMP_DIR/login-$i.status")"
  case "$status" in
    401) login_unauthorized=$((login_unauthorized + 1)) ;;
    429) login_limited=$((login_limited + 1)) ;;
    *)
      echo "rate limits test failed: concurrent login $i returned $status" >&2
      exit 1
      ;;
  esac
done
if [[ "$login_unauthorized" != "8" || "$login_limited" != "8" ]]; then
  echo "rate limits test failed: expected 8 accepted and 8 limited logins, got $login_unauthorized/$login_limited" >&2
  exit 1
fi

# One guest cookie is valid on both instances because they share the signing
# key. Alternating creation requests proves the database, not process memory,
# owns the 40-request decision.
SESSION_RESP="$(curl --noproxy '*' -sS -c "$COOKIE_JAR" "$BASE_A/api/session")"
CSRF_TOKEN="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$SESSION_RESP")"
PAYLOAD='{"title":"ngs-ratetest-diagram","description":"","nodes":[],"edges":[]}'

for i in $(seq 1 41); do
  if (( i % 2 == 0 )); then
    base="$BASE_B"
  else
    base="$BASE_A"
  fi
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
    -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF_TOKEN" \
    -H 'Content-Type: application/json' \
    -X POST -d "$PAYLOAD" "$base/api/diagrams")"
  if (( i <= 40 )) && [[ "$status" != "201" ]]; then
    echo "rate limits test failed: create $i returned $status, expected 201" >&2
    exit 1
  fi
  if (( i == 41 )) && [[ "$status" != "429" ]]; then
    echo "rate limits test failed: shared create bucket returned $status, expected 429" >&2
    exit 1
  fi
done

# A full process stop/start must not reset either decision.
stop_instances
start_instance "$ENV_A" "$PORT_A" "$LOG_A"

login_status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data 'username=invalid&password=invalid' "$BASE_A/login")"
if [[ "$login_status" != "429" ]]; then
  echo "rate limits test failed: login bucket reset after restart ($login_status)" >&2
  exit 1
fi

create_status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H 'Content-Type: application/json' \
  -X POST -d "$PAYLOAD" "$BASE_A/api/diagrams")"
if [[ "$create_status" != "429" ]]; then
  echo "rate limits test failed: create bucket reset after restart ($create_status)" >&2
  exit 1
fi

echo "persistent shared rate limits test passed"
