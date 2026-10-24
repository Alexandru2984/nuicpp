#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "security smoke"

PORT="$("$ROOT_DIR/scripts/find_free_port.sh" 18151 18250)"
TMP_DIR="$(mktemp -d)"
TMP_ENV="$TMP_DIR/.env"
COOKIE_JAR="$TMP_DIR/cookies.txt"
LOG_FILE="$TMP_DIR/server.log"
PID=""

TEST_TITLE_PREFIX="ngs-smoke-"
# Filled in once the guest session exists. Cleaning by owner rather than by
# title matters here: when an authorization check regresses, this run creates a
# copy of somebody else's diagram, which carries their title and would survive a
# prefix-based sweep.
TEST_OWNER_HASH=""

cleanup() {
  if [[ -n "$PID" ]] && kill -0 "$PID" >/dev/null 2>&1; then
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "$TEST_OWNER_HASH" ]]; then
    psql --dbname="$DATABASE_URL" -X -tAc \
      "DELETE FROM diagrams WHERE owner_token_hash = '${TEST_OWNER_HASH}'" >/dev/null 2>&1 || true
  fi
  psql --dbname="$DATABASE_URL" -X -tAc \
    "DELETE FROM diagrams WHERE title LIKE '${TEST_TITLE_PREFIX}%'" >/dev/null 2>&1 || true
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
  echo "security smoke failed: server did not become healthy" >&2
  cat "$LOG_FILE" >&2
  exit 1
fi

session_json="$(curl --noproxy '*' -fsS -c "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/session")"
guest_user="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$session_json")"
# Same derivation as ownerHashForRequest(): HMAC-SHA256(OWNER_HASH_SECRET, "owner:" + username).
TEST_OWNER_HASH="$(printf 'owner:%s' "$guest_user" \
  | openssl dgst -sha256 -hmac "$(live_test_env_value OWNER_HASH_SECRET)" -r | cut -d' ' -f1)"

diagrams_json="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/diagrams")"
python3 - "$diagrams_json" <<'PY'
import json
import sys

payload = json.loads(sys.argv[1])
diagrams = payload.get("diagrams")
if not isinstance(diagrams, list):
    raise SystemExit("diagrams response is not a list")
if diagrams:
    raise SystemExit("new guest session can enumerate existing diagrams")
PY

CSRF="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/session" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["csrf_token"])')"

legacy_session="$(awk '$6 == "ngs_session" { value=$7 } END { print value }' "$COOKIE_JAR")"
if [[ -z "$legacy_session" ]]; then
  echo "security smoke failed: local session cookie was not issued" >&2
  exit 1
fi
secure_session_headers="$TMP_DIR/secure-session.headers"
secure_session_body="$TMP_DIR/secure-session.json"
curl --noproxy '*' -fsS -D "$secure_session_headers" -o "$secure_session_body" \
  -H 'X-Forwarded-Proto: https' -H "Cookie: ngs_session=${legacy_session}" \
  "http://127.0.0.1:${PORT}/api/session"
secure_username="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["username"])' "$secure_session_body")"
if [[ "$secure_username" != "$guest_user" ]]; then
  echo "legacy HTTPS session migration changed the guest identity" >&2
  exit 1
fi
if ! grep -Eqi '^set-cookie: __Host-ngs_session=[^;]+; Path=/; Max-Age=[1-9][0-9]*; HttpOnly; SameSite=Strict; Secure' "$secure_session_headers"; then
  echo "HTTPS session was not upgraded to a __Host- cookie" >&2
  exit 1
fi
if ! grep -Fqi 'set-cookie: ngs_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict; Secure; Expires=Thu, 01 Jan 1970 00:00:00 GMT' "$secure_session_headers"; then
  echo "legacy HTTPS session cookie was not expired during upgrade" >&2
  exit 1
fi
host_session="$(sed -n 's/^Set-Cookie: __Host-ngs_session=\([^;]*\).*/\1/p' "$secure_session_headers" | tail -n 1)"
if [[ -z "$host_session" ]]; then
  echo "security smoke failed: upgraded host session value is missing" >&2
  exit 1
fi

precedence_json="$(curl --noproxy '*' -fsS -H 'X-Forwarded-Proto: https' \
  -H "Cookie: ngs_session=invalid; __Host-ngs_session=${host_session}" \
  "http://127.0.0.1:${PORT}/api/session")"
precedence_username="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$precedence_json")"
if [[ "$precedence_username" != "$guest_user" ]]; then
  echo "__Host- session cookie did not take precedence over the legacy name" >&2
  exit 1
fi

shadowed_json="$(curl --noproxy '*' -fsS -H 'X-Forwarded-Proto: https' \
  -H "Cookie: __Host-ngs_session=invalid; ngs_session=${legacy_session}" \
  "http://127.0.0.1:${PORT}/api/session")"
shadowed_username="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["username"])' "$shadowed_json")"
if [[ "$shadowed_username" == "$guest_user" ]]; then
  echo "invalid __Host- cookie unexpectedly fell back to a legacy session" >&2
  exit 1
fi

secure_logout_headers="$TMP_DIR/secure-logout.headers"
status="$(curl --noproxy '*' -sS -D "$secure_logout_headers" -o /dev/null -w '%{http_code}' \
  -H 'X-Forwarded-Proto: https' -H 'Content-Type: application/json' \
  -H "Cookie: __Host-ngs_session=${host_session}; ngs_session=${legacy_session}" \
  -X POST -d '' "http://127.0.0.1:${PORT}/logout")"
if [[ "$status" != "302" ]]; then
  echo "expected secure logout to return 302, got $status" >&2
  exit 1
fi
if [[ "$(grep -ci '^set-cookie:' "$secure_logout_headers")" != "2" ]] ||
   ! grep -Fqi 'set-cookie: __Host-ngs_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict; Secure' "$secure_logout_headers" ||
   ! grep -Fqi 'set-cookie: ngs_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict; Secure' "$secure_logout_headers"; then
  echo "secure logout did not expire both session cookie names" >&2
  exit 1
fi

# Positive control. Without this, every 403 below could be a rejected CSRF token
# rather than the authorization check under test, and the file would pass while
# proving nothing.
own_json="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}own\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "http://127.0.0.1:${PORT}/api/diagrams")"
own_id="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$own_json")"
own_revision="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$own_json")"
own_shared_json="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${own_id}-r${own_revision}\"" \
  -X POST -d '{"action":"enable"}' \
  "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/sharing")"
own_share_slug="$(python3 -c 'import json,sys; data=json.loads(sys.argv[1]); assert data["sharing_enabled"] is True; print(data["slug"])' "$own_shared_json")"
own_shared_revision="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["revision"])' "$own_shared_json")"
curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H "If-Match: \"diagram-${own_id}-r${own_shared_revision}\"" \
  -X POST -d '{"action":"set_expiry","expires_in":3600}' \
  "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/sharing" >/dev/null

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H 'Sec-Fetch-Site: same-origin' \
  -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "200" && "$status" != "201" ]]; then
  echo "expected duplicate of the visitor's own diagram to succeed, got $status" >&2
  exit 1
fi

existing_id="$(psql --dbname="$DATABASE_URL" -X -tAc "select min(id) from diagrams where title not like '${TEST_TITLE_PREFIX}%'" | tr -d '[:space:]')"
if [[ -n "$existing_id" ]]; then
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}")"
  if [[ "$status" != "403" ]]; then
    echo "expected direct diagram id read to return 403, got $status" >&2
    exit 1
  fi

  # Duplicate returns a full owned copy of the source, so it is a read of that
  # source and needs the same check as GET. It used to have none, which handed
  # any visitor any diagram by id.
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
    -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}/duplicate")"
  if [[ "$status" != "403" ]]; then
    echo "expected duplicate of another visitor's diagram to return 403, got $status" >&2
    exit 1
  fi

  # A version snapshot is a whole diagram at a past moment, so reading one is
  # reading the diagram. The ownership check runs before the version lookup, so
  # this is 403 whether or not that version number exists.
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    "http://127.0.0.1:${PORT}/api/diagrams/${existing_id}/versions/1")"
  if [[ "$status" != "403" ]]; then
    echo "expected another visitor's version snapshot to return 403, got $status" >&2
    exit 1
  fi
fi

# Positive control for the same route: the visitor's own snapshot is readable,
# so the 403 above is the ownership check answering and not a routing accident.
own_version="$(curl --noproxy '*' -fsS -b "$COOKIE_JAR" \
  "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/versions" \
  | python3 -c 'import json,sys; vs=json.load(sys.stdin)["versions"]; print(vs[0]["id"] if vs else "")')"
if [[ -n "$own_version" ]]; then
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/versions/${own_version}")"
  if [[ "$status" != "200" ]]; then
    echo "expected the visitor's own version snapshot to return 200, got $status" >&2
    exit 1
  fi
else
  echo "security smoke failed: own diagram has no version to read" >&2
  exit 1
fi

# The route regex matches \d+ with no length bound, so an id past LONG_MAX used
# to reach std::stol and throw out of the handler.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  "http://127.0.0.1:${PORT}/api/diagrams/99999999999999999999")"
if [[ "$status" != "400" ]]; then
  echo "expected out-of-range diagram id to return 400, got $status" >&2
  exit 1
fi

# Content-Type matters: cpp-httplib rejects a bodyless POST that carries no
# content type with a 400 before routing, which would pass this check for the
# wrong reason. Send a well-formed request so the CSRF gate is what answers.
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  -X POST -d '{"title":"csrf probe","description":"","nodes":[],"edges":[]}' \
  "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "403" ]]; then
  echo "expected mutating request without CSRF to return 403, got $status" >&2
  exit 1
fi

# Fetch Metadata is an early browser-only boundary in front of login, CSRF,
# parsing and rate limits. A real browser does not allow page JavaScript to
# forge Sec-Fetch-Site, while CLI clients that omit it remain compatible.
fetch_headers="$TMP_DIR/fetch-metadata.headers"
fetch_body="$TMP_DIR/fetch-metadata.json"
status="$(curl --noproxy '*' -sS -D "$fetch_headers" -o "$fetch_body" -w '%{http_code}' \
  -H 'Sec-Fetch-Site: cross-site' -H 'Content-Type: application/x-www-form-urlencoded' \
  -X POST -d 'username=ignored&password=ignored' \
  "http://127.0.0.1:${PORT}/login")"
if [[ "$status" != "403" ]]; then
  echo "expected cross-site login submission to return 403, got $status" >&2
  exit 1
fi
for expected in \
  'content-type: application/json' \
  'cache-control: private, no-store, max-age=0' \
  'x-content-type-options: nosniff'
do
  if ! grep -Fqi "$expected" "$fetch_headers"; then
    echo "expected $expected on a Fetch Metadata rejection" >&2
    exit 1
  fi
done
python3 - "$fetch_body" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as response:
    assert json.load(response) == {"error": "cross-site request blocked"}
PY

for fetch_site in cross-site same-site none; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
    -H "Sec-Fetch-Site: ${fetch_site}" \
    -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}fetch-${fetch_site}\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
    "http://127.0.0.1:${PORT}/api/diagrams")"
  if [[ "$status" != "403" ]]; then
    echo "expected ${fetch_site} mutation to return 403, got $status" >&2
    exit 1
  fi
done

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Sec-Fetch-Site: cross-site' "http://127.0.0.1:${PORT}/live")"
if [[ "$status" != "200" ]]; then
  echo "expected cross-site safe read to remain available, got $status" >&2
  exit 1
fi

origin_headers="$TMP_DIR/origin.headers"
origin_body="$TMP_DIR/origin.json"
status="$(curl --noproxy '*' -sS -D "$origin_headers" -o "$origin_body" -w '%{http_code}' \
  -b "$COOKIE_JAR" -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H 'Origin: https://foreign.example' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}foreign-origin\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "403" ]]; then
  echo "expected foreign request origin to return 403, got $status" >&2
  exit 1
fi
for expected in \
  'content-type: application/json' \
  'cache-control: private, no-store, max-age=0' \
  'x-content-type-options: nosniff'
do
  if ! grep -Fqi "$expected" "$origin_headers"; then
    echo "expected $expected on an origin rejection" >&2
    exit 1
  fi
done
python3 - "$origin_body" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as response:
    assert json.load(response) == {"error": "request origin blocked"}
PY

for origin in null https://same-site.example; do
  status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
    -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' -H "Origin: ${origin}" \
    -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
  if [[ "$status" != "403" ]]; then
    echo "expected origin ${origin} to return 403, got $status" >&2
    exit 1
  fi
done

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H "Origin: http://127.0.0.1:${PORT}" -H 'Origin: https://foreign.example' \
  -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "403" ]]; then
  echo "expected duplicate Origin headers to return 403, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H "Origin: http://127.0.0.1:${PORT}" \
  -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "201" ]]; then
  echo "expected configured request origin to be accepted, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Origin: https://foreign.example' "http://127.0.0.1:${PORT}/live")"
if [[ "$status" != "200" ]]; then
  echo "expected foreign Origin to be ignored on a safe read, got $status" >&2
  exit 1
fi

media_headers="$TMP_DIR/media-type.headers"
media_body="$TMP_DIR/media-type.json"
status="$(curl --noproxy '*' -sS -D "$media_headers" -o "$media_body" -w '%{http_code}' \
  -b "$COOKIE_JAR" -H "X-CSRF-Token: $CSRF" -H 'Content-Type: text/plain' \
  -X POST -d "{\"title\":\"${TEST_TITLE_PREFIX}wrong-media\",\"description\":\"\",\"nodes\":[],\"edges\":[]}" \
  "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "415" ]]; then
  echo "expected non-JSON API body to return 415, got $status" >&2
  exit 1
fi
for expected in \
  'content-type: application/json' \
  'cache-control: private, no-store, max-age=0' \
  'x-content-type-options: nosniff'
do
  if ! grep -Fqi "$expected" "$media_headers"; then
    echo "expected $expected on an unsupported media type response" >&2
    exit 1
  fi
done
python3 - "$media_body" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as response:
    assert json.load(response) == {"error": "unsupported media type"}
PY

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/json' -X POST -d '{}' \
  "http://127.0.0.1:${PORT}/login")"
if [[ "$status" != "415" ]]; then
  echo "expected JSON login body to return 415, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: application/json' \
  -H 'Content-Type: text/plain' -X POST -d '{}' \
  "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "415" ]]; then
  echo "expected conflicting Content-Type headers to return 415, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H "X-CSRF-Token: $CSRF" -H 'Content-Type: Application/JSON; charset=utf-8' \
  -X POST -d '{}' "http://127.0.0.1:${PORT}/api/diagrams/${own_id}/duplicate")"
if [[ "$status" != "201" ]]; then
  echo "expected parameterized JSON media type to be accepted, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'Content-Type: application/x-www-form-urlencoded; charset=UTF-8' \
  -X POST -d 'username=ignored&password=ignored' \
  "http://127.0.0.1:${PORT}/login")"
if [[ "$status" != "401" ]]; then
  echo "expected parameterized form media type to reach login, got $status" >&2
  exit 1
fi

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" \
  -H 'Content-Type: text/plain' "http://127.0.0.1:${PORT}/api/diagrams")"
if [[ "$status" != "200" ]]; then
  echo "expected Content-Type to be ignored on a bodyless safe read, got $status" >&2
  exit 1
fi

# The app sets these itself rather than depending on the vhost staying correct,
# so they must be present even when nginx is not in the path.
for probe in health live ready; do
  probe_json="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/${probe}")"
  python3 -c 'import json,sys; assert json.loads(sys.argv[1])["status"] in {"live", "ready"}' "$probe_json"
done

metrics_headers="$TMP_DIR/metrics.headers"
metrics_body="$(curl --noproxy '*' -fsS -D "$metrics_headers" \
  "http://127.0.0.1:${PORT}/metrics")"
for metric in \
  nuigraph_build_info \
  nuigraph_http_requests_total \
  nuigraph_http_request_duration_seconds \
  nuigraph_security_events_total \
  nuigraph_diagram_creates_total \
  nuigraph_database_up \
  nuigraph_storage_capacity \
  nuigraph_logical_storage_bytes \
  nuigraph_logical_storage_capacity_bytes \
  nuigraph_database_size_bytes \
  nuigraph_db_pool_timeouts_total
do
  if ! grep -q "^# HELP ${metric}\|^${metric}" <<<"$metrics_body"; then
    echo "expected aggregate metric $metric" >&2
    exit 1
  fi
done
if ! grep -q '^nuigraph_database_up 1$' <<<"$metrics_body"; then
  echo "metrics scrape did not confirm a working database connection" >&2
  exit 1
fi
if ! grep -Eq '^nuigraph_storage_capacity\{kind="diagrams"\} [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "metrics scrape did not expose the configured diagram capacity" >&2
  exit 1
fi
if ! grep -Eq '^nuigraph_logical_storage_capacity_bytes [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "metrics scrape did not expose the configured logical storage capacity" >&2
  exit 1
fi
for series in \
  'nuigraph_security_events_total{event="share_view",outcome="allowed"}' \
  'nuigraph_security_events_total{event="share_view",outcome="blocked"}' \
  'nuigraph_security_events_total{event="sharing_change",outcome="enabled"}' \
  'nuigraph_security_events_total{event="sharing_change",outcome="disabled"}' \
  'nuigraph_security_events_total{event="sharing_change",outcome="rotated"}' \
  'nuigraph_security_events_total{event="sharing_change",outcome="expiry_updated"}' \
  'nuigraph_security_events_total{event="fetch_metadata_rejected",outcome="blocked"}' \
  'nuigraph_security_events_total{event="origin_rejected",outcome="blocked"}' \
  'nuigraph_security_events_total{event="unsupported_media_type",outcome="blocked"}' \
  'nuigraph_security_events_total{event="session_cookie_upgrade",outcome="succeeded"}' \
  'nuigraph_security_events_total{event="session_rekey",outcome="succeeded"}'
do
  if ! grep -Fq "$series" <<<"$metrics_body"; then
    echo "expected bounded sharing metric $series" >&2
    exit 1
  fi
done
if ! grep -Eq '^nuigraph_security_events_total\{event="fetch_metadata_rejected",outcome="blocked"\} [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "Fetch Metadata rejections were not counted" >&2
  exit 1
fi
if ! grep -Eq '^nuigraph_security_events_total\{event="origin_rejected",outcome="blocked"\} [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "request origin rejections were not counted" >&2
  exit 1
fi
if ! grep -Eq '^nuigraph_security_events_total\{event="unsupported_media_type",outcome="blocked"\} [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "unsupported media type rejections were not counted" >&2
  exit 1
fi
if ! grep -Eq '^nuigraph_security_events_total\{event="session_cookie_upgrade",outcome="succeeded"\} [1-9][0-9]*$' <<<"$metrics_body"; then
  echo "legacy session cookie upgrades were not counted" >&2
  exit 1
fi
if ! grep -qi '^content-type: text/plain; version=0.0.4; charset=utf-8' "$metrics_headers"; then
  echo "metrics endpoint returned the wrong content type" >&2
  exit 1
fi
if ! grep -Eqi '^cache-control:.*(^|[[:space:],])no-store([[:space:],]|$)' "$metrics_headers"; then
  echo "metrics endpoint must not be cached" >&2
  exit 1
fi
if grep -Eq 'guest_[0-9a-f]+|not-a-real-share|ci_admin' <<<"$metrics_body"; then
  echo "metrics endpoint leaked actor or bearer data" >&2
  exit 1
fi
status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  -H 'X-Real-IP: 198.51.100.20' "http://127.0.0.1:${PORT}/metrics")"
if [[ "$status" != "404" ]]; then
  echo "expected a proxied public metrics request to be hidden, got $status" >&2
  exit 1
fi

headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}/live")"
for header in \
  "content-security-policy" \
  "x-frame-options" \
  "x-content-type-options" \
  "x-permitted-cross-domain-policies" \
  "referrer-policy" \
  "origin-agent-cluster" \
  "cross-origin-opener-policy" \
  "cross-origin-resource-policy" \
  "permissions-policy" \
  "x-request-id"
do
  if ! grep -qi "^${header}:" <<<"$headers"; then
    echo "expected $header on responses from the app itself" >&2
    exit 1
  fi
done

correlation_headers="$(curl --noproxy '*' -sS -D - -o /dev/null \
  -H 'X-Request-ID: smoke-request-1234' "http://127.0.0.1:${PORT}/live")"
if ! grep -qi '^x-request-id: smoke-request-1234' <<<"$correlation_headers"; then
  echo "expected a valid trusted request id to be echoed" >&2
  exit 1
fi

if grep -qi "^content-security-policy:.*unsafe-inline" <<<"$headers"; then
  echo "content security policy must not allow unsafe-inline" >&2
  exit 1
fi
for directive in "font-src 'self'" "media-src 'none'" "frame-src 'none'" "worker-src 'self'" "manifest-src 'self'"; do
  if ! grep -Fqi "$directive" <<<"$headers"; then
    echo "content security policy is missing $directive" >&2
    exit 1
  fi
done
if ! grep -Fqi 'permissions-policy: accelerometer=(), camera=(), clipboard-read=(), clipboard-write=(self)' <<<"$headers"; then
  echo "permissions policy must deny clipboard reads and scope writes to self" >&2
  exit 1
fi

root_html="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/")"
for marker in 'name="description"' 'rel="canonical"' 'property="og:title"' 'property="og:locale"' 'name="twitter:card"' 'schema.org/SoftwareApplication' 'itemprop="applicationCategory"' '<h1>NuiGraph Studio</h1>'; do
  if ! grep -q "$marker" <<<"$root_html"; then
    echo "expected SEO marker $marker on the public editor" >&2
    exit 1
  fi
done

review_script="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/diagram-review.js")"
if ! grep -q 'NuiGraphReview' <<<"$review_script"; then
  echo "expected the local diagram review engine to be served" >&2
  exit 1
fi

arrange_script="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/diagram-arrange.js")"
if ! grep -q 'NuiGraphArrange' <<<"$arrange_script"; then
  echo "expected the local diagram arrangement engine to be served" >&2
  exit 1
fi

selection_script="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/diagram-selection.js")"
if ! grep -q 'NuiGraphSelection' <<<"$selection_script"; then
  echo "expected the local diagram selection engine to be served" >&2
  exit 1
fi

robots="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/robots.txt")"
for rule in 'Disallow: /api/' 'Disallow: /d/' 'Disallow: /login' 'Disallow: /metrics' 'Sitemap: http://127.0.0.1:'; do
  if ! grep -q "$rule" <<<"$robots"; then
    echo "expected robots.txt rule: $rule" >&2
    exit 1
  fi
done

sitemap="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/sitemap.xml")"
python3 - "$sitemap" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.fromstring(sys.argv[1])
locations = {element.text for element in root.findall("{http://www.sitemaps.org/schemas/sitemap/0.9}url/{http://www.sitemaps.org/schemas/sitemap/0.9}loc")}
if not any(location and location.endswith("/") for location in locations):
    raise SystemExit("sitemap is missing the editor URL")
if not any(location and location.endswith("/docs") for location in locations):
    raise SystemExit("sitemap is missing the documentation URL")
PY

for private_path in /login /d/not-a-real-share; do
  private_headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}${private_path}")"
  if ! grep -qi '^x-robots-tag:.*noindex' <<<"$private_headers"; then
    echo "expected X-Robots-Tag noindex on $private_path" >&2
    exit 1
  fi
done

status="$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${PORT}/d/not-a-real-share")"
if [[ "$status" != "404" ]]; then
  echo "expected an inactive share page to return 404, got $status" >&2
  exit 1
fi

share_html="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/d/${own_share_slug}")"
if ! grep -q 'name="robots" content="noindex, nofollow, noarchive"' <<<"$share_html"; then
  echo "expected an HTML noindex directive on bearer share pages" >&2
  exit 1
fi

for sensitive_path in / /login /docs "/d/${own_share_slug}" /api/session /live /ready /metrics; do
  sensitive_headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}${sensitive_path}")"
  for expected in \
    "cache-control: private, no-store, max-age=0" \
    "cdn-cache-control: no-store" \
    "cloudflare-cdn-cache-control: no-store" \
    "pragma: no-cache" \
    "expires: 0" \
    "vary: Cookie"
  do
    if ! grep -Fqi "${expected}" <<<"$sensitive_headers"; then
      echo "expected $expected on sensitive path $sensitive_path" >&2
      exit 1
    fi
  done
done

api_headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}/api/session")"
if ! grep -qi '^x-robots-tag:.*noindex' <<<"$api_headers"; then
  echo "expected X-Robots-Tag noindex on JSON API responses" >&2
  exit 1
fi

sw_headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}/sw.js")"
for expected in \
  "cache-control: private, no-store, max-age=0" \
  "cdn-cache-control: no-store" \
  "cloudflare-cdn-cache-control: no-store"
do
  if ! grep -qi "^${expected}" <<<"$sw_headers"; then
    echo "expected $expected on the service worker" >&2
    exit 1
  fi
done

asset_headers="$(curl --noproxy '*' -sS -D - -o /dev/null "http://127.0.0.1:${PORT}/styles.css")"
for expected in \
  "cache-control: public, no-cache, must-revalidate" \
  "cdn-cache-control: no-cache" \
  "cloudflare-cdn-cache-control: no-cache"
do
  if ! grep -qi "^${expected}" <<<"$asset_headers"; then
    echo "expected $expected on revalidated static assets" >&2
    exit 1
  fi
done

root_html="$(curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/")"
if [[ "$root_html" =~ /styles[.]css\?v=([0-9a-f]{16}) ]]; then
  asset_version="${BASH_REMATCH[1]}"
else
  echo "editor shell is missing its content-derived asset version" >&2
  exit 1
fi

versioned_headers="$(curl --noproxy '*' -sS -D - -o /dev/null \
  "http://127.0.0.1:${PORT}/styles.css?v=${asset_version}")"
for expected in \
  "cache-control: public, max-age=31536000, immutable" \
  "cdn-cache-control: public, max-age=31536000, immutable" \
  "cloudflare-cdn-cache-control: public, max-age=31536000, immutable"
do
  if ! grep -qi "^${expected}" <<<"$versioned_headers"; then
    echo "expected $expected on content-versioned static assets" >&2
    exit 1
  fi
done

worker_body="$(curl --noproxy '*' -fsS \
  "http://127.0.0.1:${PORT}/sw.js?v=${asset_version}")"
if ! grep -q "const ASSET_VERSION = \"${asset_version}\"" <<<"$worker_body"; then
  echo "service worker does not share the shell asset version" >&2
  exit 1
fi
if grep -q '__ASSET_VERSION__' <<<"$worker_body"; then
  echo "service worker exposed an unresolved asset-version placeholder" >&2
  exit 1
fi

if ! curl --noproxy '*' -fsS "http://127.0.0.1:${PORT}/offline.js" >/dev/null; then
  echo "offline script is not served" >&2
  exit 1
fi

if ! grep -q 'category=security event=csrf_rejected outcome=blocked' "$LOG_FILE"; then
  echo "expected an explicit CSRF rejection security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=fetch_metadata_rejected outcome=blocked' "$LOG_FILE"; then
  echo "expected an explicit Fetch Metadata rejection security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=origin_rejected outcome=blocked' "$LOG_FILE"; then
  echo "expected an explicit request origin rejection security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=unsupported_media_type outcome=blocked' "$LOG_FILE"; then
  echo "expected an explicit unsupported media type security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=session_cookie_upgrade outcome=succeeded' "$LOG_FILE"; then
  echo "expected an explicit legacy session-cookie upgrade event" >&2
  exit 1
fi
if ! grep -q 'category=security event=share_view outcome=allowed.*path=/d/<redacted>' "$LOG_FILE"; then
  echo "expected a redacted share-view security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=share_view outcome=blocked.*path=/d/<redacted>' "$LOG_FILE"; then
  echo "expected a redacted blocked share-view security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=sharing_change outcome=enabled.*path=/api/diagrams/' "$LOG_FILE"; then
  echo "expected an explicit sharing lifecycle security event" >&2
  exit 1
fi
if ! grep -q 'category=security event=sharing_change outcome=expiry_updated.*path=/api/diagrams/' "$LOG_FILE"; then
  echo "expected an explicit share-expiry lifecycle event" >&2
  exit 1
fi
if grep -q 'not-a-real-share' "$LOG_FILE"; then
  echo "bearer share value leaked into application logs" >&2
  exit 1
fi
if ! grep -q 'level=info msg=listening .* frontend=server' "$LOG_FILE"; then
  echo "expected the selected frontend shell in the startup event" >&2
  exit 1
fi
if ! grep -Eq 'request_id=[A-Za-z0-9_-]{8,64}' "$LOG_FILE"; then
  echo "expected request ids in application logs" >&2
  exit 1
fi

echo "security smoke passed"
