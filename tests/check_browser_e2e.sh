#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"
PLAYWRIGHT="$ROOT_DIR/nui-frontend/node_modules/.bin/playwright"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "browser end-to-end"

if [[ ! -x "$PLAYWRIGHT" ]]; then
  live_test_skip_or_fail "browser end-to-end" "run npm ci --prefix nui-frontend first"
fi

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18601 18700)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
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
  /^APP_BASE_URL=/ { print "APP_BASE_URL=http://127.0.0.1:" port; next }
  { print }
' "$ENV_FILE" > "$TMP_ENV"

env -i PATH="$PATH" "$BINARY" "$TMP_ENV" >"$LOG_FILE" 2>&1 &
PID="$!"

healthy=0
for _ in $(seq 1 150); do
  if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/ready" >/dev/null 2>&1; then
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
  echo "browser end-to-end failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

NUIGRAPH_BROWSER_BASE_URL="http://127.0.0.1:${PORT}" \
NUIGRAPH_BROWSER_OUTPUT_DIR="$TMP_DIR/test-results" \
  "$PLAYWRIGHT" test --config "$ROOT_DIR/nui-frontend/playwright.config.cjs"

echo "browser end-to-end passed"
