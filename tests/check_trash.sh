#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "trash lifecycle"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18951 19050)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
OWNER_COOKIES="$TMP_DIR/owner.cookies"
OTHER_COOKIES="$TMP_DIR/other.cookies"
PID=""
TEST_TITLE_PREFIX="ngs-trash-${PORT}-"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
TEST_SIGNING_SECRET="$(openssl rand -hex 32)"
TEST_CSRF_SECRET="$(openssl rand -hex 32)"
TEST_OWNER_SECRET="$(openssl rand -hex 32)"
TEST_RATE_LIMIT_SECRET="$(openssl rand -hex 32)"
IDEMPOTENCY_KEY="ngs-trash-$(openssl rand -hex 16)"

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  psql --dbname="$DATABASE_URL" -X -qtAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

BASELINE_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"

cat > "$TMP_ENV" <<ENV_EOF
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
    echo "trash lifecycle failed: server crashed during startup" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
if [[ "$healthy" != "1" ]]; then
  echo "trash lifecycle failed: server did not become healthy" >&2
  exit 1
fi

OWNER_SESSION="$(curl --noproxy '*' -fsS -c "$OWNER_COOKIES" "${BASE}/api/session")"
OWNER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OWNER_SESSION")"
OTHER_SESSION="$(curl --noproxy '*' -fsS -c "$OTHER_COOKIES" "${BASE}/api/session")"
OTHER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OTHER_SESSION")"
PAYLOAD="{\"title\":\"${TEST_TITLE_PREFIX}private\",\"description\":\"trash lifecycle\",\"nodes\":[],\"edges\":[]}"

CREATE="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $IDEMPOTENCY_KEY" -X POST -d "$PAYLOAD" \
  "${BASE}/api/diagrams")"
ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$CREATE")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$CREATE")"
AFTER_CREATE_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"
if [[ "$AFTER_CREATE_BYTES" -le "$BASELINE_BYTES" ]]; then
  echo "trash lifecycle failed: create did not increase logical usage" >&2
  exit 1
fi

ENABLED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${ID}-r${REVISION}\"" \
  -X POST -d '{"action":"enable"}' "${BASE}/api/diagrams/${ID}/sharing")"
ACTIVE_REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$ENABLED")"
SHARE_SLUG="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["slug"])' "$ENABLED")"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -X DELETE "${BASE}/api/diagrams/${ID}")"
[[ "$status" == "428" ]] || { echo "trash lifecycle failed: missing If-Match returned $status" >&2; exit 1; }

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIES" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H "If-Match: \"diagram-${ID}-r${ACTIVE_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${ID}")"
[[ "$status" == "403" ]] || { echo "trash lifecycle failed: cross-owner trash returned $status" >&2; exit 1; }

status="$(curl --noproxy '*' -sS -o "$TMP_DIR/stale.json" -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H "If-Match: \"diagram-${ID}-r${REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${ID}")"
[[ "$status" == "412" ]] || { echo "trash lifecycle failed: stale trash returned $status" >&2; exit 1; }

curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H "If-Match: \"diagram-${ID}-r${ACTIVE_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${ID}" >/dev/null

TRASH_REVISION="$((ACTIVE_REVISION + 1))"
for hidden_path in \
  "/api/diagrams/${ID}" \
  "/api/diagrams/${ID}/versions" \
  "/api/diagrams/${ID}/export.json"
do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
    "${BASE}${hidden_path}")"
  [[ "$status" == "403" ]] || {
    echo "trash lifecycle failed: hidden path $hidden_path returned $status" >&2
    exit 1
  }
done
for revoked_path in "/api/diagrams/slug/${SHARE_SLUG}" "/d/${SHARE_SLUG}"; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIES" \
    "${BASE}${revoked_path}")"
  [[ "$status" == "404" ]] || {
    echo "trash lifecycle failed: revoked share path $revoked_path returned $status" >&2
    exit 1
  }
done

OWNER_LIST="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" "${BASE}/api/diagrams")"
OWNER_TRASH="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" "${BASE}/api/trash")"
OTHER_TRASH="$(curl --noproxy '*' -fsS -b "$OTHER_COOKIES" "${BASE}/api/trash")"
python3 -c 'import json,sys; assert int(sys.argv[2]) not in [d["id"] for d in json.loads(sys.argv[1])["diagrams"]]' \
  "$OWNER_LIST" "$ID"
python3 -c 'import json,sys; items=json.loads(sys.argv[1])["diagrams"]; d=next(x for x in items if x["id"]==int(sys.argv[2])); assert d["revision"]==int(sys.argv[3]); assert d["deleted_at"] and d["slug"]=="" and d["sharing_enabled"] is False' \
  "$OWNER_TRASH" "$ID" "$TRASH_REVISION"
python3 -c 'import json,sys; assert int(sys.argv[2]) not in [d["id"] for d in json.loads(sys.argv[1])["diagrams"]]' \
  "$OTHER_TRASH" "$ID"

RETAINED_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"
[[ "$RETAINED_BYTES" == "$AFTER_CREATE_BYTES" ]] || {
  echo "trash lifecycle failed: soft delete changed logical usage" >&2
  exit 1
}

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $IDEMPOTENCY_KEY" -X POST -d "$PAYLOAD" \
  "${BASE}/api/diagrams")"
[[ "$status" == "409" ]] || { echo "trash lifecycle failed: retry of trashed result returned $status" >&2; exit 1; }

for action in restore delete; do
  if [[ "$action" == "restore" ]]; then
    method=POST
    path="/api/trash/${ID}/restore"
    content=(-H 'Content-Type: application/json' -d '{}')
  else
    method=DELETE
    path="/api/trash/${ID}"
    content=()
  fi
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIES" \
    -H "X-CSRF-Token: $OTHER_CSRF" -H "If-Match: \"diagram-${ID}-r${TRASH_REVISION}\"" \
    "${content[@]}" -X "$method" "${BASE}${path}")"
  [[ "$status" == "403" ]] || { echo "trash lifecycle failed: cross-owner $action returned $status" >&2; exit 1; }
done

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d '{}' "${BASE}/api/trash/${ID}/restore")"
[[ "$status" == "428" ]] || { echo "trash lifecycle failed: restore without If-Match returned $status" >&2; exit 1; }

RESTORED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${ID}-r${TRASH_REVISION}\"" \
  -X POST -d '{}' "${BASE}/api/trash/${ID}/restore")"
RESTORED_REVISION="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["sharing_enabled"] is False and d["slug"]==""; print(d["revision"])' "$RESTORED")"
[[ "$RESTORED_REVISION" == "$((TRASH_REVISION + 1))" ]] || {
  echo "trash lifecycle failed: restore did not increment revision" >&2
  exit 1
}
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OTHER_COOKIES" \
  "${BASE}/api/diagrams/slug/${SHARE_SLUG}")"
[[ "$status" == "404" ]] || { echo "trash lifecycle failed: restore revived old share" >&2; exit 1; }

status="$(curl --noproxy '*' -sS -D "$TMP_DIR/replay.headers" -o "$TMP_DIR/replay.json" \
  -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $IDEMPOTENCY_KEY" -X POST -d "$PAYLOAD" \
  "${BASE}/api/diagrams")"
[[ "$status" == "201" ]] || { echo "trash lifecycle failed: restored result did not replay" >&2; exit 1; }
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert d["id"]==int(sys.argv[2]) and d["revision"]==int(sys.argv[3])' \
  "$TMP_DIR/replay.json" "$ID" "$RESTORED_REVISION"
tr -d '\r' <"$TMP_DIR/replay.headers" | grep -Fqx 'Idempotency-Replayed: true'

curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H "If-Match: \"diagram-${ID}-r${RESTORED_REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${ID}" >/dev/null
FINAL_TRASH_REVISION="$((RESTORED_REVISION + 1))"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H "If-Match: \"diagram-${ID}-r${RESTORED_REVISION}\"" \
  -X DELETE "${BASE}/api/trash/${ID}")"
[[ "$status" == "412" ]] || { echo "trash lifecycle failed: stale permanent delete returned $status" >&2; exit 1; }

curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H "If-Match: \"diagram-${ID}-r${FINAL_TRASH_REVISION}\"" \
  -X DELETE "${BASE}/api/trash/${ID}" >/dev/null

ROW_COUNT="$(psql --dbname="$DATABASE_URL" -X -Atqc "SELECT count(*) FROM diagrams WHERE id=${ID}")"
FINAL_BYTES="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  'SELECT logical_bytes FROM logical_storage_usage WHERE singleton=true')"
[[ "$ROW_COUNT" == "0" && "$FINAL_BYTES" == "$BASELINE_BYTES" ]] || {
  echo "trash lifecycle failed: permanent delete did not release retained data" >&2
  exit 1
}

METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
for outcome in moved restored permanently_deleted; do
  grep -Eq "^nuigraph_security_events_total\\{event=\"diagram_trash\",outcome=\"${outcome}\"\\} [1-9][0-9]*$" <<<"$METRICS" || {
    echo "trash lifecycle failed: missing $outcome metric" >&2
    exit 1
  }
done
grep -Fqx 'nuigraph_storage_objects{kind="trashed_diagrams"} 0' <<<"$METRICS"
grep -q 'category=security event=diagram_trash outcome=permanently_deleted' "$LOG_FILE"
if grep -Fq "$IDEMPOTENCY_KEY" "$LOG_FILE"; then
  echo "trash lifecycle failed: idempotency token reached logs" >&2
  exit 1
fi

echo "trash lifecycle passed"
