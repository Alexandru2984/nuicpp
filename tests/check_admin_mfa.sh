#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "admin MFA"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 19351 19420)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
PID=""
ADMIN_PASSWORD="admin-mfa-test-password"
TOTP_SEED="GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(python3 -c 'import hashlib,sys; print(hashlib.pbkdf2_hmac("sha256", sys.argv[1].encode(), bytes.fromhex(sys.argv[2]), 150000).hex())' "$ADMIN_PASSWORD" "$TEST_AUTH_SALT")"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"

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
    "UPDATE admin_totp_state SET last_accepted_counter=-1, updated_at=NULL WHERE singleton=true; DELETE FROM rate_limit_buckets; DELETE FROM admin_sessions;" \
    >/dev/null 2>&1 || true
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
AUTH_TOTP_SECRET=$TOTP_SEED
SESSION_SIGNING_SECRET=${TEST_SIGNING_SECRET}
CSRF_SECRET=${TEST_CSRF_SECRET}
OWNER_HASH_SECRET=${TEST_OWNER_SECRET}
RATE_LIMIT_SECRET=${TEST_RATE_LIMIT_SECRET}
PUBLIC_ACCESS=false
PROJECT_ROOT=$ROOT_DIR
ENV_EOF

psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "UPDATE admin_totp_state SET last_accepted_counter=-1, updated_at=NULL WHERE singleton=true; DELETE FROM rate_limit_buckets; DELETE FROM admin_sessions;" \
  >/dev/null

start_server() {
  env -i PATH="$PATH" "$BINARY" "$TMP_ENV" >>"$LOG_FILE" 2>&1 &
  PID="$!"
  local healthy=0
  for _ in $(seq 1 150); do
    if curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/ready" >/dev/null 2>&1; then
      healthy=1
      break
    fi
    if ! kill -0 "$PID" >/dev/null 2>&1; then
      echo "admin MFA failed: server crashed during startup" >&2
      sed -n '1,160p' "$LOG_FILE" >&2
      exit 1
    fi
    sleep 0.1
  done
  [[ "$healthy" == "1" ]] || { echo "admin MFA failed: server was not ready" >&2; exit 1; }
}

start_server
BASE="http://127.0.0.1:${PORT}"

LEGACY_EXPIRY="$(( $(date +%s) + 3600 ))"
LEGACY_VALUE="$(python3 - "$TEST_SIGNING_SECRET" "$LEGACY_EXPIRY" <<'PY'
import hashlib, hmac, sys
payload = f"admin.{sys.argv[2]}"
print(f"{payload}.{hmac.new(sys.argv[1].encode(), payload.encode(), hashlib.sha256).hexdigest()}")
PY
)"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H "Cookie: ngs_session=${LEGACY_VALUE}" "${BASE}/api/session")"
[[ "$status" == "401" ]] || { echo "admin MFA failed: pre-MFA admin cookie returned $status" >&2; exit 1; }

LOGIN_PAGE="$(curl --noproxy '*' -fsS "${BASE}/login")"
grep -Fq 'name="totp"' <<<"$LOGIN_PAGE"
grep -Fq 'autocomplete="one-time-code"' <<<"$LOGIN_PAGE"
grep -Fq 'pattern="[0-9]{6}"' <<<"$LOGIN_PAGE"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
  "${BASE}/login")"
[[ "$status" == "401" ]] || { echo "admin MFA failed: missing code returned $status" >&2; exit 1; }

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
  --data-urlencode 'totp=12345x' "${BASE}/login")"
[[ "$status" == "401" ]] || { echo "admin MFA failed: invalid code returned $status" >&2; exit 1; }

read -r TOTP_COUNTER TOTP_CODE < <(python3 - "$TOTP_SEED" <<'PY'
import base64, hashlib, hmac, struct, sys, time
counter = int(time.time()) // 30
key = base64.b32decode(sys.argv[1])
digest = hmac.new(key, struct.pack(">Q", counter), hashlib.sha1).digest()
offset = digest[-1] & 15
value = struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7fffffff
print(counter, f"{value % 1_000_000:06d}")
PY
)

contender_pids=()
for contender in a b; do
  curl --noproxy '*' -sS -o "$TMP_DIR/login-${contender}.body" \
    -D "$TMP_DIR/login-${contender}.headers" -c "$TMP_DIR/${contender}.cookies" \
    -w '%{http_code}' -H 'Content-Type: application/x-www-form-urlencoded' \
    --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
    --data-urlencode "totp=${TOTP_CODE}" "${BASE}/login" \
    >"$TMP_DIR/login-${contender}.status" &
  contender_pids+=("$!")
done
for contender_pid in "${contender_pids[@]}"; do
  wait "$contender_pid"
done

statuses="$(sort "$TMP_DIR/login-a.status" "$TMP_DIR/login-b.status" | tr '\n' ' ' | sed 's/ $//')"
[[ "$statuses" == "302 401" ]] || {
  echo "admin MFA failed: concurrent code produced statuses $statuses" >&2
  exit 1
}

if grep -Fq $'\tngs_session\t' "$TMP_DIR/a.cookies"; then
  AUTHENTICATED_COOKIES="$TMP_DIR/a.cookies"
else
  AUTHENTICATED_COOKIES="$TMP_DIR/b.cookies"
fi
grep -Fq '~session~' "$AUTHENTICATED_COOKIES"
SESSION="$(curl --noproxy '*' -fsS -b "$AUTHENTICATED_COOKIES" "${BASE}/api/session")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["username"] == "admin" and "totp" not in sys.argv[1].lower()' "$SESSION"
[[ "$(psql --dbname="$DATABASE_URL" -X -Atqc 'SELECT count(*) FROM admin_sessions WHERE revoked_at IS NULL')" == "1" ]]

stored_counter="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT last_accepted_counter FROM admin_totp_state WHERE singleton=true')"
[[ "$stored_counter" == "$TOTP_COUNTER" ]] || {
  echo "admin MFA failed: replay barrier stored counter $stored_counter instead of $TOTP_COUNTER" >&2
  exit 1
}

stop_server
start_server
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
  --data-urlencode "totp=${TOTP_CODE}" "${BASE}/login")"
[[ "$status" == "401" ]] || { echo "admin MFA failed: replay after restart returned $status" >&2; exit 1; }

for candidate_name in seed code password; do
  case "$candidate_name" in
    seed) candidate="$TOTP_SEED" ;;
    code) candidate="$TOTP_CODE" ;;
    password) candidate="$ADMIN_PASSWORD" ;;
  esac
  if grep -Fq "$candidate" "$LOG_FILE"; then
    echo "admin MFA failed: ${candidate_name} reached logs" >&2
    exit 1
  fi
done

grep -Eq 'category=security event=admin_login outcome=(succeeded|failed)' "$LOG_FILE"
echo "admin MFA passed"
