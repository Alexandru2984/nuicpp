#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "share passwords"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 19251 19350)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
OWNER_COOKIES="$TMP_DIR/owner.cookies"
VISITOR_COOKIES="$TMP_DIR/visitor.cookies"
LOCKED_COOKIES="$TMP_DIR/locked.cookies"
PID=""
TEST_PREFIX="ngs-share-password-${PORT}-"
CONTENT_MARKER="private-diagram-content-${PORT}"
ADMIN_PASSWORD="share-password-test-admin"
SHARE_PASSWORD="correct-share-password"
REPLACEMENT_PASSWORD="replacement-share-password"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(python3 -c 'import hashlib,sys; print(hashlib.pbkdf2_hmac("sha256", sys.argv[1].encode(), bytes.fromhex(sys.argv[2]), 150000).hex())' "$ADMIN_PASSWORD" "$TEST_AUTH_SALT")"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  psql --dbname="$DATABASE_URL" -X -qtAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_PREFIX}%'" >/dev/null 2>&1 || true
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
SESSION_SIGNING_SECRET=${TEST_SIGNING_SECRET}
CSRF_SECRET=${TEST_CSRF_SECRET}
OWNER_HASH_SECRET=${TEST_OWNER_SECRET}
RATE_LIMIT_SECRET=${TEST_RATE_LIMIT_SECRET}
PUBLIC_ACCESS=true
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
    echo "share passwords failed: server crashed during startup" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
[[ "$healthy" == "1" ]] || { echo "share passwords failed: server was not ready" >&2; exit 1; }

OWNER_SESSION="$(curl --noproxy '*' -fsS -c "$OWNER_COOKIES" "${BASE}/api/session")"
OWNER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OWNER_SESSION")"
CREATED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d "{\"title\":\"${TEST_PREFIX}protected\",\"description\":\"${CONTENT_MARKER}\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
DIAGRAM_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$CREATED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$CREATED")"

SHARED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"enable","expires_in":3600}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
SLUG="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["slug"])' "$SHARED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$SHARED")"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"set_password","password":"short"}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
[[ "$status" == "400" ]] || { echo "share passwords failed: short password returned $status" >&2; exit 1; }

PROTECTED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d "{\"action\":\"set_password\",\"password\":\"${SHARE_PASSWORD}\"}" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["share_password_protected"] is True and "share_password_hash" not in d' "$PROTECTED"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$PROTECTED")"

STORED_HASH="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT share_password_hash FROM diagrams WHERE id=${DIAGRAM_ID}")"
[[ "$STORED_HASH" =~ ^pbkdf2_sha256\$210000\$[0-9a-f]{32}\$[0-9a-f]{64}$ ]] || {
  echo "share passwords failed: database verifier is not canonical PBKDF2" >&2
  exit 1
}
[[ "$STORED_HASH" != *"$SHARE_PASSWORD"* ]] || {
  echo "share passwords failed: plaintext password reached storage" >&2
  exit 1
}

curl --noproxy '*' -fsS -o "$TMP_DIR/export.json" -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/privacy/export"
python3 - "$TMP_DIR/export.json" "$DIAGRAM_ID" <<'PY'
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text()
data = json.loads(raw)
entry = next(item for item in data["diagrams"] if item["diagram"]["id"] == int(sys.argv[2]))
assert entry["diagram"]["share_password_protected"] is True
assert "share_password_hash" not in raw and "pbkdf2_sha256" not in raw
PY

CHALLENGE="$(curl --noproxy '*' -fsS "${BASE}/d/${SLUG}")"
grep -Fq '<h1>Protected diagram</h1>' <<<"$CHALLENGE"
if grep -Fq "$CONTENT_MARKER" <<<"$CHALLENGE"; then
  echo "share passwords failed: locked page disclosed diagram content" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  "${BASE}/api/diagrams/slug/${SLUG}")"
[[ "$status" == "401" ]] || { echo "share passwords failed: locked API returned $status" >&2; exit 1; }
LOCKED_SESSION="$(curl --noproxy '*' -fsS -c "$LOCKED_COOKIES" "${BASE}/api/session")"
LOCKED_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$LOCKED_SESSION")"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$LOCKED_COOKIES" \
  -H "X-CSRF-Token: $LOCKED_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/diagrams/slug/${SLUG}/fork")"
[[ "$status" == "401" ]] || { echo "share passwords failed: locked fork returned $status" >&2; exit 1; }
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/json' -X POST -d '{}' "${BASE}/d/${SLUG}/unlock")"
[[ "$status" == "415" ]] || { echo "share passwords failed: wrong unlock media type returned $status" >&2; exit 1; }
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode 'password=wrong-share-password' "${BASE}/d/${SLUG}/unlock")"
[[ "$status" == "401" ]] || { echo "share passwords failed: wrong password returned $status" >&2; exit 1; }

curl --noproxy '*' -sS -D "$TMP_DIR/unlock.headers" -o /dev/null \
  -c "$VISITOR_COOKIES" -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode "password=${SHARE_PASSWORD}" "${BASE}/d/${SLUG}/unlock"
tr -d '\r' <"$TMP_DIR/unlock.headers" | grep -Fqx 'HTTP/1.1 303 See Other'
tr -d '\r' <"$TMP_DIR/unlock.headers" | grep -Fqx "Location: /d/${SLUG}"
tr -d '\r' <"$TMP_DIR/unlock.headers" | grep -Eq '^Set-Cookie: ngs_share_access=[^;]+; Path=/; Max-Age=28800; HttpOnly; SameSite=Strict$'

curl --noproxy '*' -sS -D "$TMP_DIR/secure-unlock.headers" -o /dev/null \
  -H 'X-Forwarded-Proto: https' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode "password=${SHARE_PASSWORD}" "${BASE}/d/${SLUG}/unlock"
tr -d '\r' <"$TMP_DIR/secure-unlock.headers" | \
  grep -Eq '^Set-Cookie: __Host-ngs_share_access=[^;]+; Path=/; Max-Age=28800; HttpOnly; SameSite=Strict; Secure$'

ACCESS_VALUE="$(awk '$6 == "ngs_share_access" {print $7}' "$VISITOR_COOKIES")"
[[ -n "$ACCESS_VALUE" ]] || { echo "share passwords failed: unlock cookie was not persisted" >&2; exit 1; }
if [[ "${ACCESS_VALUE: -1}" == "0" ]]; then
  TAMPERED_ACCESS="${ACCESS_VALUE::-1}1"
else
  TAMPERED_ACCESS="${ACCESS_VALUE::-1}0"
fi
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H "Cookie: ngs_share_access=${TAMPERED_ACCESS}" \
  "${BASE}/api/diagrams/slug/${SLUG}")"
[[ "$status" == "401" ]] || { echo "share passwords failed: tampered unlock cookie returned $status" >&2; exit 1; }

UNLOCKED_PAGE="$(curl --noproxy '*' -fsS -b "$VISITOR_COOKIES" "${BASE}/d/${SLUG}")"
grep -Fq 'id="canvas"' <<<"$UNLOCKED_PAGE"
UNLOCKED_API="$(curl --noproxy '*' -fsS -b "$VISITOR_COOKIES" "${BASE}/api/diagrams/slug/${SLUG}")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["id"] == int(sys.argv[2]) and d["share_password_protected"] is True' \
  "$UNLOCKED_API" "$DIAGRAM_ID"

VISITOR_SESSION="$(curl --noproxy '*' -fsS -b "$VISITOR_COOKIES" -c "$VISITOR_COOKIES" "${BASE}/api/session")"
VISITOR_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$VISITOR_SESSION")"
FORKED="$(curl --noproxy '*' -fsS -b "$VISITOR_COOKIES" \
  -H "X-CSRF-Token: $VISITOR_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/diagrams/slug/${SLUG}/fork")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["id"] != int(sys.argv[2]) and d["share_password_protected"] is False' \
  "$FORKED" "$DIAGRAM_ID"

REPLACED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d "{\"action\":\"set_password\",\"password\":\"${REPLACEMENT_PASSWORD}\"}" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$REPLACED")"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$VISITOR_COOKIES" \
  "${BASE}/api/diagrams/slug/${SLUG}")"
[[ "$status" == "401" ]] || { echo "share passwords failed: old unlock survived password replacement" >&2; exit 1; }
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode "password=${SHARE_PASSWORD}" "${BASE}/d/${SLUG}/unlock")"
[[ "$status" == "401" ]] || { echo "share passwords failed: old password remained valid" >&2; exit 1; }

REMOVED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"remove_password"}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["share_password_protected"] is False' "$REMOVED"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$REMOVED")"
curl --noproxy '*' -fsS "${BASE}/api/diagrams/slug/${SLUG}" >/dev/null

PROTECTED_AGAIN="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d "{\"action\":\"set_password\",\"password\":\"${REPLACEMENT_PASSWORD}\"}" \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$PROTECTED_AGAIN")"
curl --noproxy '*' -fsS -o /dev/null -c "$VISITOR_COOKIES" \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data-urlencode "password=${REPLACEMENT_PASSWORD}" "${BASE}/d/${SLUG}/unlock"
ROTATED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"rotate"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
NEW_SLUG="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["share_password_protected"] is True; print(d["slug"])' "$ROTATED")"
[[ "$NEW_SLUG" != "$SLUG" ]] || { echo "share passwords failed: rotation reused the slug" >&2; exit 1; }
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$VISITOR_COOKIES" \
  "${BASE}/api/diagrams/slug/${NEW_SLUG}")"
[[ "$status" == "401" ]] || { echo "share passwords failed: old unlock crossed a link rotation" >&2; exit 1; }

last_status=""
for _ in $(seq 1 9); do
  last_status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    --data-urlencode 'password=repeated-wrong-password' "${BASE}/d/${NEW_SLUG}/unlock")"
done
[[ "$last_status" == "429" ]] || { echo "share passwords failed: ninth failed attempt returned $last_status" >&2; exit 1; }

METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
for series in \
  'nuigraph_security_events_total{event="sharing_change",outcome="password_updated"}' \
  'nuigraph_security_events_total{event="sharing_change",outcome="password_removed"}' \
  'nuigraph_security_events_total{event="share_password",outcome="required"}' \
  'nuigraph_security_events_total{event="share_password",outcome="unlocked"}' \
  'nuigraph_security_events_total{event="share_password",outcome="failed"}' \
  'nuigraph_security_events_total{event="share_password",outcome="rate_limited"}'
do
  grep -Eq "^${series} [1-9][0-9]*$" <<<"$METRICS" || {
    echo "share passwords failed: missing metric $series" >&2
    exit 1
  }
done
for leak_kind in password replacement verifier old_capability new_capability; do
  case "$leak_kind" in
    password) candidate="$SHARE_PASSWORD" ;;
    replacement) candidate="$REPLACEMENT_PASSWORD" ;;
    verifier) candidate="$STORED_HASH" ;;
    old_capability) candidate="$SLUG" ;;
    new_capability) candidate="$NEW_SLUG" ;;
  esac
  if grep -Fq "$candidate" "$LOG_FILE"; then
    echo "share passwords failed: ${leak_kind} reached logs" >&2
    grep -F "$candidate" "$LOG_FILE" | sed "s#${candidate}#<redacted>#g" >&2
    exit 1
  fi
done

echo "share passwords passed"
