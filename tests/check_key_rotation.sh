#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "security key rotation test"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18651 18750)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
OLD_COOKIE_JAR="$TMP_DIR/old.cookies"
UPGRADED_COOKIE_JAR="$TMP_DIR/upgraded.cookies"
PID=""
TEST_TITLE_PREFIX="ngs-key-rotation-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
OLD_SIGNING_SECRET="$(openssl rand -hex 32)"
NEW_SIGNING_SECRET="$(openssl rand -hex 32)"
OLD_CSRF_SECRET="$(openssl rand -hex 32)"
NEW_CSRF_SECRET="$(openssl rand -hex 32)"
OWNER_SECRET="$(openssl rand -hex 32)"
OLD_RATE_SECRET="$(openssl rand -hex 32)"
NEW_RATE_SECRET="$(openssl rand -hex 32)"

stop_server() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  PID=""
}

cleanup() {
  stop_server
  psql --dbname="$DATABASE_URL" -X -qtAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

write_env() {
  local signing_secret="$1"
  local previous_signing_secret="$2"
  local csrf_secret="$3"
  local rate_secret="$4"
  {
    printf '%s\n' \
      'APP_ENV=test' \
      'APP_HOST=127.0.0.1' \
      "APP_PORT=$PORT" \
      "APP_BASE_URL=http://127.0.0.1:$PORT" \
      "DATABASE_URL=$DATABASE_URL" \
      'AUTH_USERNAME=admin' \
      "AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}" \
      "SESSION_SIGNING_SECRET=$signing_secret" \
      "SESSION_SIGNING_SECRET_PREVIOUS=$previous_signing_secret" \
      "CSRF_SECRET=$csrf_secret" \
      "OWNER_HASH_SECRET=$OWNER_SECRET" \
      "RATE_LIMIT_SECRET=$rate_secret" \
      'PUBLIC_ACCESS=true' \
      "PROJECT_ROOT=$ROOT_DIR"
  } >"$TMP_ENV"
}

start_server() {
  local log_file="$1"
  local healthy=0
  env -i PATH="$PATH" "$BINARY" "$TMP_ENV" >"$log_file" 2>&1 &
  PID="$!"
  for _ in $(seq 1 150); do
    if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/ready" >/dev/null 2>&1; then
      healthy=1
      break
    fi
    if ! kill -0 "$PID" >/dev/null 2>&1; then
      echo "key rotation test failed: server crashed during startup" >&2
      cat "$log_file" >&2
      exit 1
    fi
    sleep 0.1
  done
  if [[ "$healthy" != "1" ]]; then
    echo "key rotation test failed: server did not become healthy" >&2
    cat "$log_file" >&2
    exit 1
  fi
}

cookie_value() {
  awk '$6 == "ngs_session" { value=$7 } END { print value }' "$1"
}

BASE="http://127.0.0.1:${PORT}"

# Establish a guest identity and owned diagram under the original signing key.
write_env "$OLD_SIGNING_SECRET" "" "$OLD_CSRF_SECRET" "$OLD_RATE_SECRET"
start_server "$TMP_DIR/initial.log"
INITIAL_SESSION="$(curl --noproxy '*' -fsS -c "$OLD_COOKIE_JAR" "${BASE}/api/session")"
INITIAL_USERNAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$INITIAL_SESSION")"
INITIAL_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$INITIAL_SESSION")"
INITIAL_COOKIE="$(cookie_value "$OLD_COOKIE_JAR")"
INITIAL_EXPIRY="$(cut -d. -f2 <<<"$INITIAL_COOKIE")"
CREATE_RESPONSE="$(curl --noproxy '*' -fsS -b "$OLD_COOKIE_JAR" \
  -H "X-CSRF-Token: $INITIAL_CSRF" -H 'Content-Type: application/json' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}owned\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
DIAGRAM_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$CREATE_RESPONSE")"
stop_server

# A planned rotation accepts the old signature once, preserves the username and
# ownership hash, and replaces both the cookie signature and CSRF token.
write_env "$NEW_SIGNING_SECRET" "$OLD_SIGNING_SECRET" "$NEW_CSRF_SECRET" "$NEW_RATE_SECRET"
start_server "$TMP_DIR/transition.log"
cp "$OLD_COOKIE_JAR" "$UPGRADED_COOKIE_JAR"
UPGRADED_SESSION="$(curl --noproxy '*' -fsS -b "$OLD_COOKIE_JAR" -c "$UPGRADED_COOKIE_JAR" \
  "${BASE}/api/session")"
UPGRADED_USERNAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$UPGRADED_SESSION")"
UPGRADED_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$UPGRADED_SESSION")"
UPGRADED_COOKIE="$(cookie_value "$UPGRADED_COOKIE_JAR")"
UPGRADED_EXPIRY="$(cut -d. -f2 <<<"$UPGRADED_COOKIE")"
if [[ "$UPGRADED_USERNAME" != "$INITIAL_USERNAME" ||
      "$UPGRADED_COOKIE" == "$INITIAL_COOKIE" ||
      "$UPGRADED_CSRF" == "$INITIAL_CSRF" ||
      "$UPGRADED_EXPIRY" != "$INITIAL_EXPIRY" ]]; then
  echo "key rotation test failed: transition did not preserve identity and replace credentials" >&2
  exit 1
fi
curl --noproxy '*' -fsS -b "$UPGRADED_COOKIE_JAR" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}" >/dev/null
METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
grep -Eq '^nuigraph_security_events_total\{event="session_rekey",outcome="succeeded"\} [1-9][0-9]*$' <<<"$METRICS"
grep -q 'category=security event=session_rekey outcome=succeeded' "$TMP_DIR/transition.log"
stop_server

# Retiring the previous key keeps the upgraded cookie valid and rejects the old
# signature. Public access issues the rejected client a different guest identity.
write_env "$NEW_SIGNING_SECRET" "" "$NEW_CSRF_SECRET" "$NEW_RATE_SECRET"
start_server "$TMP_DIR/retired.log"
CURRENT_SESSION="$(curl --noproxy '*' -fsS -b "$UPGRADED_COOKIE_JAR" "${BASE}/api/session")"
CURRENT_USERNAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$CURRENT_SESSION")"
if [[ "$CURRENT_USERNAME" != "$INITIAL_USERNAME" ]]; then
  echo "key rotation test failed: upgraded session failed after previous key retirement" >&2
  exit 1
fi
curl --noproxy '*' -fsS -b "$UPGRADED_COOKIE_JAR" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}" >/dev/null
REJECTED_SESSION="$(curl --noproxy '*' -fsS -b "$OLD_COOKIE_JAR" "${BASE}/api/session")"
REJECTED_USERNAME="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$REJECTED_SESSION")"
if [[ "$REJECTED_USERNAME" == "$INITIAL_USERNAME" ]]; then
  echo "key rotation test failed: retired signing key still authenticated" >&2
  exit 1
fi

echo "security key rotation test passed"
