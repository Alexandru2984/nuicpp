#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "privacy data controls"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 19151 19250)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
OWNER_COOKIES="$TMP_DIR/owner.cookies"
OTHER_COOKIES="$TMP_DIR/other.cookies"
ADMIN_COOKIES="$TMP_DIR/admin.cookies"
PID=""
TEST_PREFIX="ngs-privacy-${PORT}-"
ADMIN_PASSWORD="privacy-test-password"
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
    echo "privacy data controls failed: server crashed during startup" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
[[ "$healthy" == "1" ]] || { echo "privacy data controls failed: server was not ready" >&2; exit 1; }

OWNER_SESSION="$(curl --noproxy '*' -fsS -c "$OWNER_COOKIES" "${BASE}/api/session")"
OWNER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OWNER_SESSION")"
OTHER_SESSION="$(curl --noproxy '*' -fsS -c "$OTHER_COOKIES" "${BASE}/api/session")"
OTHER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OTHER_SESSION")"

create_diagram() {
  local cookies="$1"
  local csrf="$2"
  local title="$3"
  local key="$4"
  curl --noproxy '*' -fsS -b "$cookies" \
    -H "X-CSRF-Token: $csrf" -H 'Content-Type: application/json' \
    -H "Idempotency-Key: $key" -X POST \
    -d "{\"title\":\"${title}\",\"description\":\"private export content\",\"nodes\":[],\"edges\":[]}" \
    "${BASE}/api/diagrams"
}

ACTIVE="$(create_diagram "$OWNER_COOKIES" "$OWNER_CSRF" "${TEST_PREFIX}active" "privacy-active-${PORT}")"
ACTIVE_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$ACTIVE")"
ACTIVE_REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$ACTIVE")"
TRASHED="$(create_diagram "$OWNER_COOKIES" "$OWNER_CSRF" "${TEST_PREFIX}trash" "privacy-trash-${PORT}")"
TRASHED_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$TRASHED")"
TRASHED_REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$TRASHED")"
OTHER="$(create_diagram "$OTHER_COOKIES" "$OTHER_CSRF" "${TEST_PREFIX}other" "privacy-other-${PORT}")"
OTHER_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$OTHER")"

SHARED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${ACTIVE_ID}-r${ACTIVE_REVISION}\"" \
  -X POST -d '{"action":"enable"}' "${BASE}/api/diagrams/${ACTIVE_ID}/sharing")"
SHARE_SLUG="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["slug"])' "$SHARED")"
curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{"note":"privacy export snapshot"}' \
  "${BASE}/api/diagrams/${ACTIVE_ID}/versions" >/dev/null
curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" \
  -H "If-Match: \"diagram-${TRASHED_ID}-r${TRASHED_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${TRASHED_ID}" >/dev/null

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H 'Content-Type: application/json' -X POST -d '{}' "${BASE}/api/privacy/export")"
[[ "$status" == "403" ]] || { echo "privacy data controls failed: export without CSRF returned $status" >&2; exit 1; }

curl --noproxy '*' -fsS -D "$TMP_DIR/export.headers" -o "$TMP_DIR/export.json" \
  -b "$OWNER_COOKIES" -H "X-CSRF-Token: $OWNER_CSRF" \
  -H 'Content-Type: application/json' -X POST -d '{}' "${BASE}/api/privacy/export"
tr -d '\r' <"$TMP_DIR/export.headers" | grep -Fqx 'Content-Disposition: attachment; filename="nuigraph-owner-data.json"'
tr -d '\r' <"$TMP_DIR/export.headers" | grep -Fqi 'Cache-Control: no-store'

OWNER_HASH="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT owner_token_hash FROM diagrams WHERE id=${ACTIVE_ID}")"
python3 - "$TMP_DIR/export.json" "$ACTIVE_ID" "$TRASHED_ID" "$OTHER_ID" "$SHARE_SLUG" "$OWNER_HASH" <<'PY'
import json, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_text()
data = json.loads(raw)
active_id, trash_id, other_id = map(int, sys.argv[2:5])
share_slug, owner_hash = sys.argv[5:7]
assert data["format"] == "nuigraph-owner-export-v1"
assert data["scope"] == "guest_session"
assert data["trash_retention_days"] is None
assert owner_hash not in raw and "owner_token_hash" not in raw and "share_url" not in raw
assert "share_password_hash" not in raw
entries = {entry["diagram"]["id"]: entry for entry in data["diagrams"]}
assert set(entries) == {active_id, trash_id} and other_id not in entries
assert entries[active_id]["state"] == "active"
assert entries[active_id]["diagram"]["slug"] == share_slug
assert entries[trash_id]["state"] == "trash" and entries[trash_id]["deleted_at"]
assert entries[trash_id]["diagram"]["slug"] == ""
assert len(entries[active_id]["versions"]) >= 2
for entry in entries.values():
    for version in entry["versions"]:
        assert version["snapshot"]["slug"] == ""
        assert version["snapshot"]["sharing_enabled"] is False
        assert version["snapshot"]["share_password_protected"] is False
        assert version["snapshot"]["share_expires_at"] is None
PY

curl --noproxy '*' -fsS -o "$TMP_DIR/other-export.json" \
  -b "$OTHER_COOKIES" -H "X-CSRF-Token: $OTHER_CSRF" \
  -H 'Content-Type: application/json' -X POST -d '{}' "${BASE}/api/privacy/export"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert [x["diagram"]["id"] for x in d["diagrams"]] == [int(sys.argv[2])]' \
  "$TMP_DIR/other-export.json" "$OTHER_ID"

curl --noproxy '*' -fsS -c "$ADMIN_COOKIES" \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  -X POST --data-urlencode 'username=admin' --data-urlencode "password=${ADMIN_PASSWORD}" \
  "${BASE}/login" >/dev/null
ADMIN_SESSION="$(curl --noproxy '*' -fsS -b "$ADMIN_COOKIES" "${BASE}/api/session")"
ADMIN_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$ADMIN_SESSION")"
for operation in export data; do
  if [[ "$operation" == "export" ]]; then
    method=POST
    body='{}'
  else
    method=DELETE
    body='{"confirmation":"DELETE MY DATA"}'
  fi
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$ADMIN_COOKIES" \
    -H "X-CSRF-Token: $ADMIN_CSRF" -H 'Content-Type: application/json' \
    -X "$method" -d "$body" "${BASE}/api/privacy/${operation}")"
  [[ "$status" == "403" ]] || { echo "privacy data controls failed: admin $operation returned $status" >&2; exit 1; }
done

for confirmation in 'wrong' ''; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
    -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
    -X DELETE -d "{\"confirmation\":\"${confirmation}\"}" "${BASE}/api/privacy/data")"
  [[ "$status" == "400" ]] || { echo "privacy data controls failed: bad confirmation returned $status" >&2; exit 1; }
done

curl --noproxy '*' -fsS -D "$TMP_DIR/delete.headers" -o "$TMP_DIR/delete.json" \
  -b "$OWNER_COOKIES" -c "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X DELETE -d '{"confirmation":"DELETE MY DATA"}' "${BASE}/api/privacy/data"
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d == {"ok": True, "deleted_diagrams": 2}' \
  "$TMP_DIR/delete.json"
tr -d '\r' <"$TMP_DIR/delete.headers" | grep -Eq '^Set-Cookie: ngs_session=;.*Max-Age=0'

owner_rows="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT count(*) FROM diagrams WHERE id IN (${ACTIVE_ID}, ${TRASHED_ID})")"
other_rows="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT count(*) FROM diagrams WHERE id=${OTHER_ID}")"
ledger_rows="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT count(*) FROM diagram_idempotency_keys WHERE diagram_id IN (${ACTIVE_ID}, ${TRASHED_ID})")"
[[ "$owner_rows" == "0" && "$other_rows" == "1" && "$ledger_rows" == "0" ]] || {
  echo "privacy data controls failed: owner isolation or cascade delete failed" >&2
  exit 1
}
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  "${BASE}/api/diagrams/slug/${SHARE_SLUG}")"
[[ "$status" == "404" ]] || { echo "privacy data controls failed: deleted share still returned $status" >&2; exit 1; }

METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
for outcome in exported deleted blocked; do
  grep -Eq "^nuigraph_security_events_total\\{event=\"privacy_data\",outcome=\"${outcome}\"\\} [1-9][0-9]*$" <<<"$METRICS" || {
    echo "privacy data controls failed: missing $outcome metric" >&2
    exit 1
  }
done
grep -q 'category=security event=privacy_data outcome=deleted' "$LOG_FILE"
if grep -Fq "$OWNER_HASH" "$LOG_FILE" || grep -Fq "$TEST_PREFIX" "$LOG_FILE" || grep -Fq 'DELETE MY DATA' "$LOG_FILE"; then
  echo "privacy data controls failed: private material reached logs" >&2
  exit 1
fi

PRIVACY_PAGE="$(curl --noproxy '*' -fsS "${BASE}/privacy")"
grep -Fq '<h1>Privacy notice</h1>' <<<"$PRIVACY_PAGE"
grep -Fq 'rel="canonical"' <<<"$PRIVACY_PAGE"

echo "privacy data controls passed"
