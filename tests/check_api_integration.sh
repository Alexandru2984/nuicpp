#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "api integration"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18251 18350)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
OTHER_COOKIE_JAR="$TMP_DIR/other-cookies.txt"
LOG_FILE="$TMP_DIR/server.log"
PID=""
TEST_TITLE_PREFIX="ngs-integration-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SESSION_SECRET="$(openssl rand -hex 32)"

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  # These tests run against DATABASE_URL from .env, which is a real database.
  # Remove the rows this run created so repeated runs do not accumulate.
  if [[ -n "${TEST_TITLE_PREFIX:-}" ]]; then
    psql --dbname="$DATABASE_URL" -X -qtAc \
      "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cat > "$TMP_ENV" <<ENV_EOF
APP_ENV=test
APP_HOST=127.0.0.1
APP_PORT=$PORT
APP_BASE_URL=http://127.0.0.1:$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$150000\$${TEST_AUTH_SALT}\$${TEST_AUTH_DIGEST}
SESSION_SECRET=${TEST_SESSION_SECRET}
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
    echo "server crashed during startup:" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done

if [[ "$healthy" != "1" ]]; then
  echo "api integration failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

# Get guest session and extract CSRF token from JSON response
SESSION_RESP="$(curl --noproxy '*' -sS -c "$COOKIE_JAR" "${BASE}/api/session")"
CSRF_TOKEN="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$SESSION_RESP")"

if [[ -z "$CSRF_TOKEN" ]]; then
  echo "api integration failed: could not extract CSRF token" >&2
  echo "session response: $SESSION_RESP" >&2
  exit 1
fi

# Create a diagram
CREATE_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{"title":"ngs-integration-create","description":"auto","nodes":[{"key":"n1","type":"process","title":"Node 1","x":10,"y":10,"width":160,"height":80,"color":"#38bdf8","metadata":{}}],"edges":[]}' \
  "${BASE}/api/diagrams")"

DIAGRAM_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$CREATE_RESP")"
REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$CREATE_RESP")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is False; assert data['share_expired'] is False; assert data['share_expires_at'] is None; assert data['slug']==''; assert data['share_url']==''" "$CREATE_RESP"
PRIVATE_SLUG="$(psql --dbname="$DATABASE_URL" -X -tAc \
  "SELECT slug FROM diagrams WHERE id=${DIAGRAM_ID}" | tr -d '[:space:]')"
if [[ ! "$PRIVATE_SLUG" =~ ^[a-z0-9-]+-[0-9a-f]{32}$ ]]; then
  echo "api integration failed: stored private capability has unexpected format" >&2
  exit 1
fi
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$PRIVATE_SLUG")"
if [[ "$status" != "404" ]]; then
  echo "api integration failed: private diagram was reachable by slug ($status)" >&2
  exit 1
fi

# Verify listing includes the created diagram
LIST_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams")"
python3 -c "
import sys,json
data=json.loads(sys.argv[1])
ids=[d['id'] for d in data['diagrams']]
assert int(sys.argv[2]) in ids, f'diagram {sys.argv[2]} not in list: {ids}'
" "$LIST_RESP" "$DIAGRAM_ID"

# Verify GET by id
curl --noproxy '*' -sS -b "$COOKIE_JAR" -D "$TMP_DIR/get.headers" \
  -o "$TMP_DIR/get.json" "${BASE}/api/diagrams/$DIAGRAM_ID"
GET_RESP="$(<"$TMP_DIR/get.json")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-create'" "$GET_RESP"
if ! tr -d '\r' <"$TMP_DIR/get.headers" | grep -Fqx "ETag: \"diagram-${DIAGRAM_ID}-r${REVISION}\""; then
  echo "api integration failed: GET response did not expose the current diagram ETag" >&2
  exit 1
fi

# A mutating client must prove which revision it edited.
status="$(curl --noproxy '*' -sS -o "$TMP_DIR/missing-precondition.json" -w '%{http_code}' \
  -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X PUT -d '{"title":"ngs-integration-missing-precondition","description":"","nodes":[],"edges":[]}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID")"
if [[ "$status" != "428" ]]; then
  echo "api integration failed: update without If-Match returned $status instead of 428" >&2
  exit 1
fi

# Update diagram
UPDATE_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X PUT -d '{"title":"ngs-integration-updated","description":"updated","nodes":[{"key":"n1","type":"process","title":"Node 1","x":10,"y":10,"width":160,"height":80,"color":"#38bdf8","metadata":{}}],"edges":[]}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID")"
UPDATED_REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$UPDATE_RESP")"
if [[ "$UPDATED_REVISION" != "$((REVISION + 1))" ]]; then
  echo "api integration failed: update did not increment the diagram revision" >&2
  exit 1
fi

# Reusing the old ETag must not overwrite the winning update.
status="$(curl --noproxy '*' -sS -o "$TMP_DIR/stale-update.json" -w '%{http_code}' \
  -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X PUT -d '{"title":"ngs-integration-stale","description":"","nodes":[],"edges":[]}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID")"
if [[ "$status" != "412" ]]; then
  echo "api integration failed: stale update returned $status instead of 412" >&2
  exit 1
fi
python3 -c "import sys,json; assert json.load(open(sys.argv[1]))['current_revision'] == int(sys.argv[2])" \
  "$TMP_DIR/stale-update.json" "$UPDATED_REVISION"

# Verify update
GET_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['title']=='ngs-integration-updated'; assert data['revision']==int(sys.argv[2])" \
  "$GET_RESP" "$UPDATED_REVISION"

# Create version snapshot
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{"note":"integration snapshot"}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID/versions" >/dev/null

# List versions
VER_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID/versions")"
python3 -c "import sys,json;assert len(json.loads(sys.argv[1])['versions'])>0" "$VER_RESP"
MANUAL_VERSION_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['versions'][0]['id'])" "$VER_RESP")"

# Save once more, restore the snapshot with the current ETag, then prove the
# old restore precondition cannot be replayed.
SECOND_UPDATE_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${UPDATED_REVISION}\"" \
  -X PUT -d '{"title":"ngs-integration-after-snapshot","description":"updated","nodes":[],"edges":[]}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID")"
SECOND_REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$SECOND_UPDATE_RESP")"

RESTORE_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${SECOND_REVISION}\"" \
  -X POST -d '{}' "${BASE}/api/diagrams/$DIAGRAM_ID/restore/$MANUAL_VERSION_ID")"
RESTORED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['title']=='ngs-integration-updated'; print(data['revision'])" "$RESTORE_RESP")"
if [[ "$RESTORED_REVISION" != "$((SECOND_REVISION + 1))" ]]; then
  echo "api integration failed: restore did not increment the diagram revision" >&2
  exit 1
fi

# New diagrams are private until the owner explicitly enables a link. Sharing
# changes use the same optimistic revision contract as content changes.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -X POST -d '{"action":"enable","expires_in":3600}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
if [[ "$status" != "428" ]]; then
  echo "api integration failed: sharing change without If-Match returned $status instead of 428" >&2
  exit 1
fi

ENABLED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${RESTORED_REVISION}\"" \
  -X POST -d '{"action":"enable","expires_in":3600}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
ENABLED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is True; assert data['share_expired'] is False; assert data['share_expires_at'].endswith('Z'); print(data['revision'])" "$ENABLED_RESP")"
FIRST_SHARED_SLUG="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['share_url'].endswith('/d/'+data['slug']); print(data['slug'])" "$ENABLED_RESP")"
if [[ "$ENABLED_REVISION" != "$((RESTORED_REVISION + 1))" || "$FIRST_SHARED_SLUG" == "$PRIVATE_SLUG" ]]; then
  echo "api integration failed: enabling did not rotate the capability and revision" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$PRIVATE_SLUG")"
if [[ "$status" != "404" ]]; then
  echo "api integration failed: pre-enable capability remained valid" >&2
  exit 1
fi

OTHER_SESSION="$(curl --noproxy '*' -fsS -c "$OTHER_COOKIE_JAR" "${BASE}/api/session")"
OTHER_CSRF="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$OTHER_SESSION")"
SHARED_RESP="$(curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$FIRST_SHARED_SLUG")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['title']=='ngs-integration-updated'; assert data['can_edit'] is False" "$SHARED_RESP"

FORK_RESP="$(curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/diagrams/slug/$FIRST_SHARED_SLUG/fork")"
FORK_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$FORK_RESP")"
FORK_REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$FORK_RESP")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['title']=='ngs-integration-updated Copy'; assert data['can_edit'] is True; assert data['sharing_enabled'] is False; assert data['share_expires_at'] is None; assert data['slug']==''" "$FORK_RESP"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  "${BASE}/api/diagrams/$FORK_ID")"
if [[ "$status" != "403" ]]; then
  echo "api integration failed: source owner could read another guest's private fork ($status)" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${ENABLED_REVISION}\"" \
  -X POST -d '{"action":"disable"}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
if [[ "$status" != "403" ]]; then
  echo "api integration failed: another guest changed sharing with status $status" >&2
  exit 1
fi

# Expiry durations are a closed allowlist. Simulate database-clock passage so
# the suite does not sleep, then prove every capability path fails closed and
# re-enabling rotates instead of reviving the expired URL.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${ENABLED_REVISION}\"" \
  -X POST -d '{"action":"set_expiry","expires_in":1}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
if [[ "$status" != "400" ]]; then
  echo "api integration failed: non-allowlisted share expiry returned $status instead of 400" >&2
  exit 1
fi

psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -c \
  "UPDATE diagrams SET share_expires_at=now() - interval '1 second' WHERE id=${DIAGRAM_ID}" >/dev/null
for expired_path in "/api/diagrams/slug/$FIRST_SHARED_SLUG" "/d/$FIRST_SHARED_SLUG"; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIE_JAR" \
    "${BASE}${expired_path}")"
  if [[ "$status" != "404" ]]; then
    echo "api integration failed: expired capability path $expired_path returned $status" >&2
    exit 1
  fi
done
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/diagrams/slug/$FIRST_SHARED_SLUG/fork")"
if [[ "$status" != "404" ]]; then
  echo "api integration failed: expired capability could still be forked ($status)" >&2
  exit 1
fi

EXPIRED_OWNER_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is False; assert data['share_expired'] is True; assert data['slug']==''; assert data['share_url']==''" "$EXPIRED_OWNER_RESP"

REACTIVATED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${ENABLED_REVISION}\"" \
  -X POST -d '{"action":"enable","expires_in":3600}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
REACTIVATED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is True; assert data['share_expired'] is False; print(data['revision'])" "$REACTIVATED_RESP")"
REACTIVATED_SLUG="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['slug'])" "$REACTIVATED_RESP")"
if [[ "$REACTIVATED_REVISION" != "$((ENABLED_REVISION + 1))" || "$REACTIVATED_SLUG" == "$FIRST_SHARED_SLUG" ]]; then
  echo "api integration failed: expired share re-enable did not rotate capability and revision" >&2
  exit 1
fi
curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$REACTIVATED_SLUG" >/dev/null

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${RESTORED_REVISION}\"" \
  -X POST -d '{"action":"rotate"}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
if [[ "$status" != "412" ]]; then
  echo "api integration failed: stale share rotation returned $status instead of 412" >&2
  exit 1
fi

ROTATED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REACTIVATED_REVISION}\"" \
  -X POST -d '{"action":"rotate"}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
ROTATED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['share_expires_at'].endswith('Z'); print(data['revision'])" "$ROTATED_RESP")"
ROTATED_SLUG="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['slug'])" "$ROTATED_RESP")"
if [[ "$ROTATED_REVISION" != "$((REACTIVATED_REVISION + 1))" || "$ROTATED_SLUG" == "$REACTIVATED_SLUG" ]]; then
  echo "api integration failed: share rotation did not replace the capability" >&2
  exit 1
fi
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$REACTIVATED_SLUG")"
if [[ "$status" != "404" ]]; then
  echo "api integration failed: rotated capability remained usable" >&2
  exit 1
fi

DISABLED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${ROTATED_REVISION}\"" \
  -X POST -d '{"action":"disable"}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
DISABLED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is False; assert data['share_expired'] is False; assert data['share_expires_at'] is None; assert data['slug']==''; assert data['share_url']==''; print(data['revision'])" "$DISABLED_RESP")"
if [[ "$DISABLED_REVISION" != "$((ROTATED_REVISION + 1))" ]]; then
  echo "api integration failed: disabling did not increment the revision" >&2
  exit 1
fi
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$ROTATED_SLUG")"
if [[ "$status" != "404" ]]; then
  echo "api integration failed: disabled share remained usable" >&2
  exit 1
fi

REENABLED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${DISABLED_REVISION}\"" \
  -X POST -d '{"action":"enable"}' "${BASE}/api/diagrams/$DIAGRAM_ID/sharing")"
RESTORED_REVISION="$(python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['share_expires_at'] is None; print(data['revision'])" "$REENABLED_RESP")"
REENABLED_SLUG="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['slug'])" "$REENABLED_RESP")"
if [[ "$RESTORED_REVISION" != "$((DISABLED_REVISION + 1))" || "$REENABLED_SLUG" == "$ROTATED_SLUG" ]]; then
  echo "api integration failed: re-enable reused a revoked capability" >&2
  exit 1
fi
curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  "${BASE}/api/diagrams/slug/$REENABLED_SLUG" >/dev/null

status="$(curl --noproxy '*' -sS -o "$TMP_DIR/stale-restore.json" -w '%{http_code}' \
  -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${SECOND_REVISION}\"" \
  -X POST -d '{}' "${BASE}/api/diagrams/$DIAGRAM_ID/restore/$MANUAL_VERSION_ID")"
if [[ "$status" != "412" ]]; then
  echo "api integration failed: stale restore returned $status instead of 412" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o "$TMP_DIR/stale-delete.json" -w '%{http_code}' \
  -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${SECOND_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/$DIAGRAM_ID")"
if [[ "$status" != "412" ]]; then
  echo "api integration failed: stale delete returned $status instead of 412" >&2
  exit 1
fi

# Duplicate diagram
DUP_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{}' "${BASE}/api/diagrams/$DIAGRAM_ID/duplicate")"
DUP_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$DUP_RESP")"
DUP_REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$DUP_RESP")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-updated Copy'" "$DUP_RESP"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is False; assert data['slug']==''" "$DUP_RESP"

# Export JSON
EXP_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID/export.json")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-updated'" "$EXP_RESP"

# Import diagram
IMP_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{"title":"ngs-integration-imported","description":"","nodes":[],"edges":[]}' \
  "${BASE}/api/diagrams/import")"
IMP_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$IMP_RESP")"
IMP_REVISION="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['revision'])" "$IMP_RESP")"
python3 -c "import sys,json; data=json.loads(sys.argv[1]); assert data['sharing_enabled'] is False; assert data['slug']==''" "$IMP_RESP"

# Delete all created diagrams with their latest revisions.
delete_diagram() {
  local did="$1"
  local revision="$2"
  curl --noproxy '*' -sS -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF_TOKEN" \
    -H "If-Match: \"diagram-${did}-r${revision}\"" \
    -X DELETE "${BASE}/api/diagrams/$did" >/dev/null
}
delete_diagram "$DIAGRAM_ID" "$RESTORED_REVISION"
delete_diagram "$DUP_ID" "$DUP_REVISION"
delete_diagram "$IMP_ID" "$IMP_REVISION"
curl --noproxy '*' -sS -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" \
  -H "If-Match: \"diagram-${FORK_ID}-r${FORK_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/$FORK_ID" >/dev/null

echo "api integration passed"
