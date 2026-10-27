#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "administrator sessions"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 19421 19480)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
PID=""
ADMIN_PASSWORD="admin-session-test-password"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(python3 -c 'import hashlib,sys; print(hashlib.pbkdf2_hmac("sha256", sys.argv[1].encode(), bytes.fromhex(sys.argv[2]), 150000).hex())' "$ADMIN_PASSWORD" "$TEST_AUTH_SALT")"

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
    "DELETE FROM admin_sessions; DELETE FROM rate_limit_buckets;" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cat >"$TMP_ENV" <<ENV_EOF
APP_ENV=test
APP_HOST=127.0.0.1
APP_PORT=$PORT
APP_BASE_URL=http://127.0.0.1:$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}
SESSION_SIGNING_SECRET=$(openssl rand -hex 32)
CSRF_SECRET=$(openssl rand -hex 32)
OWNER_HASH_SECRET=$(openssl rand -hex 32)
RATE_LIMIT_SECRET=$(openssl rand -hex 32)
PUBLIC_ACCESS=false
PROJECT_ROOT=$ROOT_DIR
ENV_EOF

psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "DELETE FROM admin_sessions; DELETE FROM rate_limit_buckets;" >/dev/null

start_server() {
  env -i PATH="$PATH" "$BINARY" "$TMP_ENV" >>"$LOG_FILE" 2>&1 &
  PID="$!"
  for _ in $(seq 1 150); do
    curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/ready" >/dev/null 2>&1 && return
    kill -0 "$PID" >/dev/null 2>&1 || { sed -n '1,160p' "$LOG_FILE" >&2; exit 1; }
    sleep 0.1
  done
  echo "administrator sessions failed: server was not ready" >&2
  exit 1
}

login() {
  local label="$1" jar="$2"
  curl --noproxy '*' -fsS -o /dev/null -c "$jar" \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
    --data-urlencode "device_label=${label}" "${BASE}/login"
}

start_server
BASE="http://127.0.0.1:${PORT}"
JAR_A="$TMP_DIR/a.cookies"
JAR_B="$TMP_DIR/b.cookies"
login 'Work laptop' "$JAR_A"
login 'Phone browser' "$JAR_B"

SESSION_B="$(curl --noproxy '*' -fsS -b "$JAR_B" "${BASE}/api/session")"
CSRF_B="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$SESSION_B")"
curl --noproxy '*' -fsS -b "$JAR_B" "${BASE}/api/admin/sessions" >"$TMP_DIR/sessions.json"
read -r ID_A ID_B < <(python3 - "$TMP_DIR/sessions.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding="utf-8"))
assert data["maximum_active"] == 20 and len(data["sessions"]) == 2
assert sum(item["current"] for item in data["sessions"]) == 1
assert all(set(item) == {"id", "device_label", "created_at", "last_seen_at", "expires_at", "current"} for item in data["sessions"])
by_label = {item["device_label"]: item for item in data["sessions"]}
assert by_label["Phone browser"]["current"] is True
print(by_label["Work laptop"]["id"], by_label["Phone browser"]["id"])
PY
)

TOKEN_A="$(awk '$6 == "ngs_session" { split($7, p, "~"); print p[4] }' "$JAR_A")"
[[ "$TOKEN_A" =~ ^[0-9a-f]{64}$ ]]
[[ "$(psql --dbname="$DATABASE_URL" -X -Atqc "SELECT count(*) FROM admin_sessions WHERE token_hash='${TOKEN_A}'")" == "0" ]]

curl --noproxy '*' -fsS -b "$JAR_B" -H "X-CSRF-Token: ${CSRF_B}" \
  -X DELETE "${BASE}/api/admin/sessions/${ID_A}" >"$TMP_DIR/revoked-a.json"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d == {"ok": True, "current_revoked": False}' "$TMP_DIR/revoked-a.json"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$JAR_A" "${BASE}/api/session")"
[[ "$status" == "401" ]]

stop_server
start_server
curl --noproxy '*' -fsS -b "$JAR_B" "${BASE}/api/session" >/dev/null

SESSION_B="$(curl --noproxy '*' -fsS -b "$JAR_B" "${BASE}/api/session")"
CSRF_B="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$SESSION_B")"
curl --noproxy '*' -fsS -D "$TMP_DIR/revoke-current.headers" -o "$TMP_DIR/revoke-current.json" \
  -b "$JAR_B" -H "X-CSRF-Token: ${CSRF_B}" -X DELETE \
  "${BASE}/api/admin/sessions/${ID_B}"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d == {"ok": True, "current_revoked": True}' "$TMP_DIR/revoke-current.json"
grep -Eqi '^set-cookie: ngs_session=;.*Max-Age=0' "$TMP_DIR/revoke-current.headers"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$JAR_B" "${BASE}/api/session")"
[[ "$status" == "401" ]]
[[ "$(psql --dbname="$DATABASE_URL" -X -Atqc 'SELECT count(*) FROM admin_sessions WHERE revoked_at IS NOT NULL')" == "2" ]]

if grep -Fq "$TOKEN_A" "$LOG_FILE" || grep -Fq 'Work laptop' "$LOG_FILE"; then
  echo "administrator sessions failed: token or device label reached logs" >&2
  exit 1
fi
grep -q 'category=security event=admin_session outcome=revoked actor=admin' "$LOG_FILE"
echo "administrator sessions passed"
