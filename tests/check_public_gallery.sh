#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "public gallery"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 19551 19650)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
LOG_FILE="$TMP_DIR/server.log"
OWNER_COOKIES="$TMP_DIR/owner.cookies"
VISITOR_COOKIES="$TMP_DIR/visitor.cookies"
PID=""
TEST_PREFIX="ngs-public-${PORT}-"
ADMIN_PASSWORD="public-gallery-test-admin"
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
    echo "public gallery failed: server crashed during startup" >&2
    cat "$LOG_FILE" >&2
    exit 1
  fi
  sleep 0.1
done
[[ "$healthy" == "1" ]] || { echo "public gallery failed: server was not ready" >&2; exit 1; }

OWNER_SESSION="$(curl --noproxy '*' -fsS -c "$OWNER_COOKIES" "${BASE}/api/session")"
OWNER_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$OWNER_SESSION")"
TITLE="${TEST_PREFIX}<script>x</script>"
DESCRIPTION='"><img src=x onerror=alert(1)>'
CREATE_BODY="$(python3 -c 'import json,sys; print(json.dumps({"title":sys.argv[1],"description":sys.argv[2],"nodes":[],"edges":[]}))' "$TITLE" "$DESCRIPTION")"
CREATED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -X POST -d "$CREATE_BODY" "${BASE}/api/diagrams")"
DIAGRAM_ID="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$CREATED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$CREATED")"

python3 - "$(curl --noproxy '*' -fsS "${BASE}/api/public/diagrams")" "$TITLE" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
assert payload["total"] >= 0 and payload["limit"] == 24
assert all(item["title"] != sys.argv[2] for item in payload["diagrams"])
PY
if curl --noproxy '*' -fsS "${BASE}/gallery" | grep -Fq "$TITLE"; then
  echo "public gallery failed: private diagram appeared in gallery" >&2
  exit 1
fi

SHARED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"enable","expires_in":3600}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
SLUG="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["visibility"] == "unlisted" and not d["public_url"]; print(d["slug"])' "$SHARED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$SHARED")"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "${BASE}/p/${SLUG}")"
[[ "$status" == "404" ]] || { echo "public gallery failed: unlisted page returned $status" >&2; exit 1; }

PROTECTED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"set_password","password":"public-gallery-password"}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$PROTECTED")"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"publish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
[[ "$status" == "400" ]] || { echo "public gallery failed: password-protected publish returned $status" >&2; exit 1; }

UNPROTECTED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"remove_password"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$UNPROTECTED")"

VISITOR_SESSION="$(curl --noproxy '*' -fsS -c "$VISITOR_COOKIES" "${BASE}/api/session")"
VISITOR_CSRF="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["csrf_token"])' "$VISITOR_SESSION")"
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$VISITOR_COOKIES" \
  -H "X-CSRF-Token: $VISITOR_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"publish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
[[ "$status" == "403" ]] || { echo "public gallery failed: non-owner publish returned $status" >&2; exit 1; }

PUBLISHED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"publish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["visibility"] == "public" and d["public_url"].endswith("/p/" + d["slug"])' "$PUBLISHED"
PUBLIC_SLUG="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["slug"])' "$PUBLISHED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$PUBLISHED")"

curl --noproxy '*' -fsS -D "$TMP_DIR/public.headers" -o "$TMP_DIR/public.html" "${BASE}/p/${PUBLIC_SLUG}"
grep -Eqi '^cache-control:.*no-store' "$TMP_DIR/public.headers"
grep -Fq "rel=\"canonical\" href=\"${BASE}/p/${PUBLIC_SLUG}\"" "$TMP_DIR/public.html"
grep -Fq 'name="robots" content="index, follow"' "$TMP_DIR/public.html"
grep -Fq '&lt;script&gt;x&lt;/script&gt;' "$TMP_DIR/public.html"
grep -Fq '&quot;&gt;&lt;img src=x onerror=alert(1)&gt;' "$TMP_DIR/public.html"
if grep -Fq '<script>x</script>' "$TMP_DIR/public.html" || grep -Fq '"><img src=x' "$TMP_DIR/public.html"; then
  echo "public gallery failed: persisted metadata was not escaped" >&2
  exit 1
fi

PUBLIC_API="$(curl --noproxy '*' -fsS -D "$TMP_DIR/api.headers" "${BASE}/api/public/diagrams/slug/${PUBLIC_SLUG}")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert "id" not in d and d["visibility"] == "public" and d["can_edit"] is False and d["share_password_protected"] is False' "$PUBLIC_API"
grep -Eqi '^cache-control:.*no-store' "$TMP_DIR/api.headers"
grep -Eqi '^x-robots-tag:.*noindex' "$TMP_DIR/api.headers"
if grep -Eqi '^etag:' "$TMP_DIR/api.headers"; then
  echo "public gallery failed: public response exposed the internal diagram id through ETag" >&2
  exit 1
fi

CATALOG="$(curl --noproxy '*' -fsS "${BASE}/api/public/diagrams?limit=9999&offset=999999999")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["limit"] == 100 and d["offset"] == 100000' "$CATALOG"
CATALOG="$(curl --noproxy '*' -fsS "${BASE}/api/public/diagrams")"
python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert any(x["slug"] == sys.argv[2] and x["visibility"] == "public" for x in d["diagrams"])' "$CATALOG" "$PUBLIC_SLUG"
GALLERY="$(curl --noproxy '*' -fsS "${BASE}/gallery")"
grep -Fq "/p/${PUBLIC_SLUG}" <<<"$GALLERY"
grep -Fq '&lt;script&gt;x&lt;/script&gt;' <<<"$GALLERY"

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"set_password","password":"public-gallery-password"}' \
  "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
[[ "$status" == "400" ]] || { echo "public gallery failed: public password update returned $status" >&2; exit 1; }

UNPUBLISHED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"unpublish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
UNLISTED_SLUG="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["visibility"] == "unlisted" and not d["public_url"]; print(d["slug"])' "$UNPUBLISHED")"
[[ "$UNLISTED_SLUG" != "$PUBLIC_SLUG" ]] || { echo "public gallery failed: unpublish reused public slug" >&2; exit 1; }
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$UNPUBLISHED")"
for path in "/p/${PUBLIC_SLUG}" "/api/public/diagrams/slug/${PUBLIC_SLUG}"; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "${BASE}${path}")"
  [[ "$status" == "404" ]] || { echo "public gallery failed: revoked public path $path returned $status" >&2; exit 1; }
done
curl --noproxy '*' -fsS "${BASE}/d/${UNLISTED_SLUG}" >/dev/null

REPUBLISHED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"publish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
EXPIRING_SLUG="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["slug"])' "$REPUBLISHED")"
psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -qtAc \
  "UPDATE diagrams SET share_expires_at=statement_timestamp() - interval '1 second' WHERE id=${DIAGRAM_ID}" >/dev/null
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "${BASE}/p/${EXPIRING_SLUG}")"
[[ "$status" == "404" ]] || { echo "public gallery failed: expired public page returned $status" >&2; exit 1; }
EXPIRED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" "${BASE}/api/diagrams/${DIAGRAM_ID}")"
REVISION="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["share_expired"] is True and d["visibility"] == "private"; print(d["revision"])' "$EXPIRED")"

REENABLED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"enable","expires_in":3600}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
TRASH_SLUG="$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); assert d["visibility"] == "unlisted"; print(d["slug"])' "$REENABLED")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$REENABLED")"
REPUBLISHED="$(curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X POST -d '{"action":"publish"}' "${BASE}/api/diagrams/${DIAGRAM_ID}/sharing")"
REVISION="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$REPUBLISHED")"
curl --noproxy '*' -fsS -b "$OWNER_COOKIES" \
  -H "X-CSRF-Token: $OWNER_CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${DIAGRAM_ID}-r${REVISION}\"" \
  -X DELETE "${BASE}/api/diagrams/${DIAGRAM_ID}" >/dev/null
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "${BASE}/p/${TRASH_SLUG}")"
[[ "$status" == "404" ]] || { echo "public gallery failed: trashed public page returned $status" >&2; exit 1; }

robots="$(curl --noproxy '*' -fsS "${BASE}/robots.txt")"
grep -Fq "Sitemap: ${BASE}/sitemap.xml" <<<"$robots"
if grep -Eq '^Disallow: /(p/|gallery)' <<<"$robots"; then
  echo "public gallery failed: robots policy blocks public discovery" >&2
  exit 1
fi
sitemap="$(curl --noproxy '*' -fsS "${BASE}/sitemap.xml")"
grep -Fq "<loc>${BASE}/gallery</loc>" <<<"$sitemap"

METRICS="$(curl --noproxy '*' -fsS "${BASE}/metrics")"
for outcome in published unpublished; do
  grep -Eq "^nuigraph_security_events_total\{event=\"sharing_change\",outcome=\"${outcome}\"\} [1-9][0-9]*$" <<<"$METRICS" || {
    echo "public gallery failed: missing ${outcome} metric" >&2
    exit 1
  }
done

constraint_count="$(psql --dbname="$DATABASE_URL" -X -Atqc \
  "SELECT count(*) FROM pg_constraint WHERE conname IN ('diagrams_public_requires_sharing','diagrams_public_cannot_use_password')")"
[[ "$constraint_count" == "2" ]] || { echo "public gallery failed: database visibility constraints are missing" >&2; exit 1; }

for capability in "$PUBLIC_SLUG" "$UNLISTED_SLUG" "$EXPIRING_SLUG" "$TRASH_SLUG"; do
  if grep -Fq "$capability" "$LOG_FILE"; then
    echo "public gallery failed: a diagram slug reached application logs" >&2
    exit 1
  fi
done

echo "public gallery passed"
