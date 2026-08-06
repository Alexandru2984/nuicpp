#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="$ROOT_DIR/.env"
BINARY="$ROOT_DIR/build/nuigraph-studio"

if [[ ! -x "$BINARY" ]]; then
  echo "api integration skipped: missing build/nuigraph-studio"
  exit 0
fi

env_value() {
  local key="$1"
  if [[ -f "$ENV_FILE" ]]; then
    local value
    value="$(sed -n "s/^${key}=//p" "$ENV_FILE" | tail -n 1)"
    value="${value%\"}"
    value="${value#\"}"
    value="${value%\'}"
    value="${value#\'}"
    printf '%s' "$value"
  fi
}

DATABASE_URL="$(env_value DATABASE_URL || echo '')"
if [[ -z "$DATABASE_URL" ]]; then
  echo "api integration skipped: no DATABASE_URL in .env"
  exit 0
fi

if ! psql "$DATABASE_URL" -tAc "select 1" >/dev/null 2>&1; then
  echo "api integration skipped: database unavailable"
  exit 0
fi

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18251 18350)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
LOG_FILE="$TMP_DIR/server.log"
PID=""
TEST_TITLE_PREFIX="ngs-integration-"

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  # These tests run against DATABASE_URL from .env, which is a real database.
  # Remove the rows this run created so repeated runs do not accumulate.
  if [[ -n "${TEST_TITLE_PREFIX:-}" ]]; then
    psql "$DATABASE_URL" -qtAc \
      "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

cat > "$TMP_ENV" <<ENV_EOF
APP_HOST=127.0.0.1
APP_PORT=$PORT
DATABASE_URL=$DATABASE_URL
AUTH_USERNAME=admin
AUTH_PASSWORD_HASH=pbkdf2_sha256\$1\$aa\$bb
SESSION_SECRET=testsecretfortesting1234567890ab
PUBLIC_ACCESS=true
PROJECT_ROOT=$ROOT_DIR
ENV_EOF

env -i PATH="$PATH" HOME="$HOME" "$BINARY" "$TMP_ENV" >"$LOG_FILE" 2>&1 &
PID="$!"

BASE="http://127.0.0.1:${PORT}"

healthy=0
for _ in $(seq 1 150); do
  if curl --noproxy '*' -fsS "${BASE}/health" >/dev/null 2>&1; then
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
SLUG="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['slug'])" "$CREATE_RESP")"

# Verify listing includes the created diagram
LIST_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams")"
python3 -c "
import sys,json
data=json.loads(sys.argv[1])
ids=[d['id'] for d in data['diagrams']]
assert int(sys.argv[2]) in ids, f'diagram {sys.argv[2]} not in list: {ids}'
" "$LIST_RESP" "$DIAGRAM_ID"

# Verify GET by id
GET_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-create'" "$GET_RESP"

# Update diagram
curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X PUT -d '{"title":"ngs-integration-updated","description":"updated","nodes":[{"key":"n1","type":"process","title":"Node 1","x":10,"y":10,"width":160,"height":80,"color":"#38bdf8","metadata":{}}],"edges":[]}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID" >/dev/null

# Verify update
GET_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-updated'" "$GET_RESP"

# Create version snapshot
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{"note":"integration snapshot"}' \
  "${BASE}/api/diagrams/$DIAGRAM_ID/versions" >/dev/null

# List versions
VER_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/$DIAGRAM_ID/versions")"
python3 -c "import sys,json;assert len(json.loads(sys.argv[1])['versions'])>0" "$VER_RESP"

# Duplicate diagram
DUP_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF_TOKEN" \
  -H "Content-Type: application/json" \
  -X POST -d '{}' "${BASE}/api/diagrams/$DIAGRAM_ID/duplicate")"
DUP_ID="$(python3 -c "import sys,json;print(json.loads(sys.argv[1])['id'])" "$DUP_RESP")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-updated Copy'" "$DUP_RESP"

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

# Slug-based lookup
SLUG_RESP="$(curl --noproxy '*' -sS -b "$COOKIE_JAR" "${BASE}/api/diagrams/slug/$SLUG")"
python3 -c "import sys,json;assert json.loads(sys.argv[1])['title']=='ngs-integration-updated'" "$SLUG_RESP"

# Delete all created diagrams
for did in "$DIAGRAM_ID" "$DUP_ID" "$IMP_ID"; do
  curl --noproxy '*' -sS -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF_TOKEN" \
    -X DELETE "${BASE}/api/diagrams/$did" >/dev/null
done

echo "api integration passed"
