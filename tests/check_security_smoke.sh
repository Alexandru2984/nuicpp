#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT_DIR/.env"
BINARY="$ROOT_DIR/build/nuigraph-studio"

if [[ ! -f "$ENV_FILE" || ! -x "$BINARY" ]]; then
  echo "security smoke skipped: missing .env or build/nuigraph-studio"
  exit 0
fi

env_value() {
  local key="$1"
  local value
  value="$(sed -n "s/^${key}=//p" "$ENV_FILE" | tail -n 1)"
  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"
  printf '%s' "$value"
}

DATABASE_URL="$(env_value DATABASE_URL)"

if ! psql "$DATABASE_URL" -tAc "select 1" >/dev/null 2>&1; then
  echo "security smoke skipped: database unavailable"
  exit 0
fi

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18151 18250)"
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

awk -v port="$PORT" '
  /^APP_HOST=/ { print "APP_HOST=127.0.0.1"; next }
  /^APP_PORT=/ { print "APP_PORT=" port; next }
  { print }
' "$ENV_FILE" > "$TMP_ENV"

env -i PATH="$PATH" HOME="$HOME" "$BINARY" "$TMP_ENV" >"$LOG_FILE" 2>&1 &
PID="$!"

healthy=0
for _ in $(seq 1 150); do
  if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
    healthy=1
    break
  fi
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ "$healthy" != "1" ]]; then
  echo "security smoke failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

curl --noproxy '*' -fsS -c "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/session" >/dev/null

diagrams_json="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/diagrams")"
python3 - "$diagrams_json" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
diagrams = payload.get("diagrams")
if not isinstance(diagrams, list):
    raise SystemExit("diagrams response is not a list")
if diagrams:
    raise SystemExit("new guest session can enumerate existing diagrams")
PY

existing_id="$(psql "$DATABASE_URL" -tAc "select min(id) from diagrams" | tr -d '[:space:]')"
if [[ -n "$existing_id" ]]; then
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}")"
  if [[ "$status" != "403" ]]; then
    echo "expected direct diagram id read to return 403, got $status" >&2
    exit 1
  fi
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" -X POST "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "403" ]]; then
  echo "expected mutating request without CSRF to return 403, got $status" >&2
  exit 1
fi

echo "security smoke passed"
