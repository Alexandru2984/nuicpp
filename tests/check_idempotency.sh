#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "idempotent creates"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18951 19050)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
OTHER_COOKIE_JAR="$TMP_DIR/other-cookies.txt"
LOG_FILE="$TMP_DIR/server.log"
PID=""
TEMPLATE_ID=""
TEST_TITLE_PREFIX="ngs-idempotency-${PORT}-"
TEST_KEY="ngs-itest-${PORT}-same-request"
TEST_AUTH_SALT="$(openssl rand -hex 16)"
TEST_AUTH_DIGEST="$(openssl rand -hex 32)"
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
    "DELETE FROM diagrams
     WHERE title LIKE '${TEST_TITLE_PREFIX}%'
        OR id = ${TEMPLATE_ID:-0}" >/dev/null 2>&1 || true
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
    echo "idempotent creates failed: server crashed during startup" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
if [[ "$healthy" != "1" ]]; then
  echo "idempotent creates failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

SESSION_RESP="$(curl --noproxy '*' -fsS -c "$COOKIE_JAR" "${BASE}/api/session")"
CSRF_TOKEN="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$SESSION_RESP")"
USERNAME="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['username'])" "$SESSION_RESP")"
CREATE_BODY="{\"title\":\"${TEST_TITLE_PREFIX}parallel\",\"description\":\"retry-safe\",\"nodes\":[],\"edges\":[]}"

# All contenders carry the same session, operation, key and exact request
# material. The database advisory lock must make one writer win and every
# waiter replay that committed result.
request_pids=()
for request in $(seq 1 8); do
  curl --noproxy '*' -sS -b "$COOKIE_JAR" \
    -D "$TMP_DIR/parallel-${request}.headers" \
    -o "$TMP_DIR/parallel-${request}.json" \
    -w '%{http_code}' \
    -H "X-CSRF-Token: $CSRF_TOKEN" \
    -H 'Content-Type: application/json' \
    -H "Idempotency-Key: $TEST_KEY" \
    -X POST -d "$CREATE_BODY" "${BASE}/api/diagrams" \
    >"$TMP_DIR/parallel-${request}.status" &
  request_pids+=("$!")
done
for request_pid in "${request_pids[@]}"; do
  wait "$request_pid"
done

FIRST_ID=""
replayed=0
for request in $(seq 1 8); do
  status="$(<"$TMP_DIR/parallel-${request}.status")"
  if [[ "$status" != "201" ]]; then
    echo "idempotent creates failed: parallel request $request returned $status" >&2
    cat "$TMP_DIR/parallel-${request}.json" >&2
    exit 1
  fi
  diagram_id="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" \
    "$TMP_DIR/parallel-${request}.json")"
  if [[ -z "$FIRST_ID" ]]; then
    FIRST_ID="$diagram_id"
  elif [[ "$diagram_id" != "$FIRST_ID" ]]; then
    echo "idempotent creates failed: parallel requests produced different diagrams" >&2
    exit 1
  fi
  if tr -d '\r' <"$TMP_DIR/parallel-${request}.headers" | \
      grep -Fqx 'Idempotency-Replayed: true'; then
    replayed=$((replayed + 1))
  fi
done
if [[ "$replayed" != "7" ]]; then
  echo "idempotent creates failed: expected seven replay responses, found $replayed" >&2
  exit 1
fi

created_rows="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT count(*) FROM diagrams WHERE title='${TEST_TITLE_PREFIX}parallel'")"
if [[ "$created_rows" != "1" ]]; then
  echo "idempotent creates failed: parallel retry created $created_rows rows" >&2
  exit 1
fi

metrics="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
if ! grep -Fqx 'nuigraph_diagram_creates_total{source="blank"} 1' <<<"$metrics"; then
  echo "idempotent creates failed: replay changed the creation metric" >&2
  exit 1
fi

# A later exact retry returns the original representation and marks the replay.
status="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -D "$TMP_DIR/replay.headers" -o "$TMP_DIR/replay.json" -w '%{http_code}' \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d "$CREATE_BODY" \
  "${BASE}/api/diagrams")"
REPLAY_ID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" "$TMP_DIR/replay.json")"
if [[ "$status" != "201" || "$REPLAY_ID" != "$FIRST_ID" ]] ||
   ! tr -d '\r' <"$TMP_DIR/replay.headers" | grep -Fqx 'Idempotency-Replayed: true'; then
  echo "idempotent creates failed: exact retry did not replay the original result" >&2
  exit 1
fi

# Reusing an active key with different material is a conflict, not a second
# mutation and not a reflection of either request body.
status="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -o "$TMP_DIR/conflict.json" -w '%{http_code}' \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST \
  -d "{\"title\":\"${TEST_TITLE_PREFIX}conflict\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
if [[ "$status" != "409" ]]; then
  echo "idempotent creates failed: conflicting reuse returned $status instead of 409" >&2
  exit 1
fi

# Header smuggling and ambiguous duplicate fields fail before storage.
status="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H 'Idempotency-Key: short' -X POST -d "$CREATE_BODY" "${BASE}/api/diagrams")"
if [[ "$status" != "400" ]]; then
  echo "idempotent creates failed: short key returned $status instead of 400" >&2
  exit 1
fi
status="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -H 'Idempotency-Key: another-valid-key' \
  -X POST -d "$CREATE_BODY" "${BASE}/api/diagrams")"
if [[ "$status" != "400" ]]; then
  echo "idempotent creates failed: duplicate key headers returned $status instead of 400" >&2
  exit 1
fi

# Expired keys can be reused, and cleanup touches only this principal/key.
psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -q -c \
  "UPDATE diagram_idempotency_keys
   SET created_at=statement_timestamp() - interval '25 hours',
       expires_at=statement_timestamp() - interval '1 hour'
   WHERE diagram_id=${FIRST_ID}" >/dev/null
EXPIRED_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST \
  -d "{\"title\":\"${TEST_TITLE_PREFIX}after-expiry\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
EXPIRED_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$EXPIRED_RESP")"
if [[ "$EXPIRED_ID" == "$FIRST_ID" ]]; then
  echo "idempotent creates failed: expired key replayed its old result" >&2
  exit 1
fi

# The same token is intentionally independent across creation operations.
TEMPLATE_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/templates/cloud-architecture/create")"
TEMPLATE_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$TEMPLATE_RESP")"
if [[ -z "$TEMPLATE_ID" ]]; then
  echo "idempotent creates failed: operation-scoped key was rejected" >&2
  exit 1
fi
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -D "$TMP_DIR/template-replay.headers" -o "$TMP_DIR/template-replay.json" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/templates/cloud-architecture/create"
TEMPLATE_REPLAY_ID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" \
  "$TMP_DIR/template-replay.json")"
if [[ "$TEMPLATE_REPLAY_ID" != "$TEMPLATE_ID" ]] ||
   ! tr -d '\r' <"$TMP_DIR/template-replay.headers" | grep -Fqx 'Idempotency-Replayed: true'; then
  echo "idempotent creates failed: template retry did not replay" >&2
  exit 1
fi

# The same token in a different signed guest session is also independent.
OTHER_SESSION="$(curl --noproxy '*' -fsS -c "$OTHER_COOKIE_JAR" "${BASE}/api/session")"
OTHER_CSRF="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['csrf_token'])" "$OTHER_SESSION")"
OTHER_RESP="$(curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST \
  -d "{\"title\":\"${TEST_TITLE_PREFIX}other-principal\",\"nodes\":[],\"edges\":[]}" \
  "${BASE}/api/diagrams")"
OTHER_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$OTHER_RESP")"
if [[ "$OTHER_ID" == "$EXPIRED_ID" ]]; then
  echo "idempotent creates failed: key leaked across principals" >&2
  exit 1
fi

# Duplicate, import and public fork use distinct request material but the same
# operation-scoped token. Each route must preserve the same replay contract.
DUP_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/diagrams/${FIRST_ID}/duplicate")"
DUP_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$DUP_RESP")"
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -D "$TMP_DIR/duplicate-replay.headers" -o "$TMP_DIR/duplicate-replay.json" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/diagrams/${FIRST_ID}/duplicate"
DUP_REPLAY_ID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" \
  "$TMP_DIR/duplicate-replay.json")"
if [[ "$DUP_REPLAY_ID" != "$DUP_ID" ]] ||
   ! tr -d '\r' <"$TMP_DIR/duplicate-replay.headers" | grep -Fqx 'Idempotency-Replayed: true'; then
  echo "idempotent creates failed: duplicate retry did not replay" >&2
  exit 1
fi

IMPORT_BODY="{\"title\":\"${TEST_TITLE_PREFIX}import\",\"description\":\"\",\"nodes\":[],\"edges\":[]}"
IMP_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d "$IMPORT_BODY" \
  "${BASE}/api/diagrams/import")"
IMP_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$IMP_RESP")"
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -D "$TMP_DIR/import-replay.headers" -o "$TMP_DIR/import-replay.json" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d "$IMPORT_BODY" \
  "${BASE}/api/diagrams/import"
IMP_REPLAY_ID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" \
  "$TMP_DIR/import-replay.json")"
if [[ "$IMP_REPLAY_ID" != "$IMP_ID" ]] ||
   ! tr -d '\r' <"$TMP_DIR/import-replay.headers" | grep -Fqx 'Idempotency-Replayed: true'; then
  echo "idempotent creates failed: import retry did not replay" >&2
  exit 1
fi

SHARE_RESP="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${FIRST_ID}-r1\"" -X POST -d '{"action":"enable"}' \
  "${BASE}/api/diagrams/${FIRST_ID}/sharing")"
SHARE_SLUG="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['slug'])" "$SHARE_RESP")"
FORK_RESP="$(curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/diagrams/slug/${SHARE_SLUG}/fork")"
FORK_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$FORK_RESP")"
curl --noproxy '*' -fsS -b "$OTHER_COOKIE_JAR" \
  -D "$TMP_DIR/fork-replay.headers" -o "$TMP_DIR/fork-replay.json" \
  -H "X-CSRF-Token: $OTHER_CSRF" -H 'Content-Type: application/json' \
  -H "Idempotency-Key: $TEST_KEY" -X POST -d '{}' \
  "${BASE}/api/diagrams/slug/${SHARE_SLUG}/fork"
FORK_REPLAY_ID="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['id'])" \
  "$TMP_DIR/fork-replay.json")"
if [[ "$FORK_REPLAY_ID" != "$FORK_ID" ]] ||
   ! tr -d '\r' <"$TMP_DIR/fork-replay.headers" | grep -Fqx 'Idempotency-Replayed: true'; then
  echo "idempotent creates failed: fork retry did not replay" >&2
  exit 1
fi

# Only purpose-specific HMACs are retained. The raw guest identity and request
# bodies must not appear in the retry ledger.
ledger="$(psql --dbname="$DATABASE_URL" -X -AtF '|' -c \
  "SELECT count(*), count(DISTINCT principal_key), count(DISTINCT operation),
          count(DISTINCT key_hash), count(DISTINCT request_hash),
          bool_and(principal_key ~ '^[0-9a-f]{64}$'),
          bool_and(key_hash ~ '^[0-9a-f]{64}$'),
          bool_and(request_hash ~ '^[0-9a-f]{64}$'),
          bool_and(expires_at > created_at)
   FROM diagram_idempotency_keys
   WHERE diagram_id IN (${EXPIRED_ID}, ${TEMPLATE_ID}, ${OTHER_ID},
                        ${DUP_ID}, ${IMP_ID}, ${FORK_ID})")"
if [[ "$ledger" != "6|2|5|1|6|t|t|t|t" ]]; then
  echo "idempotent creates failed: unexpected ledger state $ledger" >&2
  exit 1
fi
retained_ledger="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT principal_key || key_hash || request_hash FROM diagram_idempotency_keys
   WHERE diagram_id IN (${EXPIRED_ID}, ${TEMPLATE_ID}, ${OTHER_ID},
                        ${DUP_ID}, ${IMP_ID}, ${FORK_ID})")"
if grep -Fq "$USERNAME" <<<"$retained_ledger" || grep -Fq "$TEST_KEY" <<<"$retained_ledger"; then
  echo "idempotent creates failed: raw client data reached the ledger" >&2
  exit 1
fi
if psql --dbname="$DATABASE_URL" -X -Atqc \
    "SELECT column_name FROM information_schema.columns
     WHERE table_schema='public' AND table_name='diagram_idempotency_keys'" | \
    grep -Fqx 'idempotency_key'; then
  echo "idempotent creates failed: schema retained raw idempotency tokens" >&2
  exit 1
fi

metrics="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
for security_metric in \
  'event="idempotency_rejected",outcome="blocked"' \
  'event="idempotency_conflict",outcome="blocked"'; do
  if ! grep -Eq "^nuigraph_security_events_total\\{${security_metric}\\} [1-9][0-9]*$" <<<"$metrics"; then
    echo "idempotent creates failed: missing bounded security metric $security_metric" >&2
    exit 1
  fi
done
if grep -Fq "$TEST_KEY" "$LOG_FILE"; then
  echo "idempotent creates failed: raw idempotency token reached logs" >&2
  exit 1
fi

echo "idempotent creates passed"
