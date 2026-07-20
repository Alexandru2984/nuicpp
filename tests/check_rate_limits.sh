#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT_DIR/.env"
BINARY="$ROOT_DIR/build/nuigraph-studio"

if [[ ! -x "$BINARY" ]]; then
  echo "rate limits test skipped: missing build/nuigraph-studio"
  exit 0
fi

env_value() {
  local key="$1"
  if [[ -f "$ENV_FILE" ]]; then
    local value
    value="$(sed -n "s/^${key}=//p" "$ENV_FILE" | tail -n 1)"
    value="${value%\"}"
    value="${value#\"}"
    value="${value%\'}"
    value="${value#\'}"
    printf '%s' "$value"
  fi
}

DATABASE_URL="$(env_value DATABASE_URL || echo '')"
if [[ -z "$DATABASE_URL" ]]; then
  echo "rate limits test skipped: no DATABASE_URL in .env"
  exit 0
fi

if ! psql "$DATABASE_URL" -tAc "select 1" >/dev/null 2>&1; then
  echo "rate limits test skipped: database unavailable"
  exit 0
fi

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18351 18450)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
LOG_FILE="$TMP_DIR/server.log"
PID=""

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cat > "$TMP_ENV" <<ENV_EOF
APP_HOST=127.0.0.1
APP_PORT=$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$1\$aa\$bb
SESSION_SECRET=testsecretfortesting1234567890ab
PUBLIC_ACCESS=true
PROJECT_ROOT=$ROOT_DIR
ENV_EOF

env -i PATH="$PATH" HOME="$HOME" "$BINARY" "$TMP_ENV" >"$LOG_FILE" 2>&1 &
PID="$!"

BASE="http://127.0.0.1:${PORT}"

healthy=0
for _ in $(seq 1 150); do
  if curl --noproxy '*' -fsS "${BASE}/health" >/dev/null 2>&1; then
    healthy=1
    break
  fi
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "server crashed during startup:" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ "$healthy" != "1" ]]; then
  echo "rate limits test failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

# Get guest session + CSRF token
SESSION_RESP="$(curl --noproxy '*' -sS -c "$COOKIE_JAR" "${BASE}/api/session")"
CSRF_TOKEN="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$SESSION_RESP")"

PAYLOAD='{"title":"Rate Test","description":"","nodes":[],"edges":[]}'

got_429=0
for i in $(seq 1 60); do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
    -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF_TOKEN" \
    -H "Content-Type: application/json" \
    -X POST -d "$PAYLOAD" "${BASE}/api/diagrams")"
  if [[ "$status" == "429" ]]; then
    got_429=1
    echo "rate limit hit after $i requests"
    break
  fi
done

if [[ "$got_429" != "1" ]]; then
  echo "rate limits test failed: never received 429" >&2
  exit 1
fi

echo "rate limits test passed"
