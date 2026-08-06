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

TEST_TITLE_PREFIX="ngs-smoke-"

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  # Remove the rows this run created so repeated runs do not accumulate.
  psql "$DATABASE_URL" -tAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
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

CSRF="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/session" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["csrf_token"])')"

# Positive control. Without this, every 403 below could be a rejected CSRF token
# rather than the authorization check under test, and the file would pass while
# proving nothing.
own_json="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}own\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "http://127.0.0.1:${PORT}/api/diagrams")"
own_id="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$own_json")"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "200" && "$status" != "201" ]]; then
  echo "expected duplicate of the visitor's own diagram to succeed, got $status" >&2
  exit 1
fi

existing_id="$(psql "$DATABASE_URL" -tAc "select min(id) from diagrams where title not like '${TEST_TITLE_PREFIX}%'" | tr -d '[:space:]')"
if [[ -n "$existing_id" ]]; then
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}")"
  if [[ "$status" != "403" ]]; then
    echo "expected direct diagram id read to return 403, got $status" >&2
    exit 1
  fi

  # Duplicate returns a full owned copy of the source, so it is a read of that
  # source and needs the same check as GET. It used to have none, which handed
  # any visitor any diagram by id.
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
    -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}/duplicate")"
  if [[ "$status" != "403" ]]; then
    echo "expected duplicate of another visitor's diagram to return 403, got $status" >&2
    exit 1
  fi
fi

# The route regex matches \d+ with no length bound, so an id past LONG_MAX used
# to reach std::stol and throw out of the handler.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  "http://127.0.0.1:${PORT}/api/diagrams/99999999999999999999")"
if [[ "$status" != "400" ]]; then
  echo "expected out-of-range diagram id to return 400, got $status" >&2
  exit 1
fi

# Content-Type matters: cpp-httplib rejects a bodyless POST that carries no
# content type with a 400 before routing, which would pass this check for the
# wrong reason. Send a well-formed request so the CSRF gate is what answers.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  -X POST -d '{"title":"csrf probe","description":"","nodes":[],"edges":[]}' \
  "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "403" ]]; then
  echo "expected mutating request without CSRF to return 403, got $status" >&2
  exit 1
fi

# The app sets these itself rather than depending on the vhost staying correct,
# so they must be present even when nginx is not in the path.
headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}/health")"
for header in \
  "content-security-policy" \
  "x-frame-options" \
  "x-content-type-options" \
  "referrer-policy" \
  "cross-origin-opener-policy" \
  "cross-origin-resource-policy" \
  "permissions-policy"
do
  if ! grep -qi "^${header}:" <<<"$headers"; then
    echo "expected $header on responses from the app itself" >&2
    exit 1
  fi
done

if grep -qi "^content-security-policy:.*unsafe-inline" <<<"$(grep -i '^content-security-policy:' <<<"$headers" | sed 's/style-src[^;]*;//')"; then
  echo "script policy must not allow unsafe-inline" >&2
  exit 1
fi

echo "security smoke passed"
