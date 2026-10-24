#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VHOST="$ROOT_DIR/deploy/nginx/nuicpp.micutu.com.conf"
RATE_LIMITS="$ROOT_DIR/deploy/nginx/nuicpp-rate-limit.conf"

for command in nginx openssl sed; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "nginx config test failed: missing command $command" >&2
    exit 1
  fi
done

require_text() {
  local file="$1"
  local text="$2"
  if ! grep -Fq "$text" "$file"; then
    echo "nginx config test failed: $file is missing: $text" >&2
    exit 1
  fi
}

require_count() {
  local file="$1"
  local text="$2"
  local expected="$3"
  local actual
  actual="$(grep -Fc "$text" "$file" || true)"
  if [[ "$actual" != "$expected" ]]; then
    echo "nginx config test failed: expected $expected occurrences of '$text' in $file, found $actual" >&2
    exit 1
  fi
}

# These assertions protect the security properties that a syntax-only test
# cannot understand. ACME must bypass only the app origin guard, every proxied
# route must carry correlation, and bearer capabilities must use the redacted
# log format.
require_count "$VHOST" 'location ^~ /.well-known/acme-challenge/' 2
require_text "$VHOST" "set \$nuicpp_origin_allowed \$from_cloudflare_origin;"
require_count "$VHOST" "proxy_set_header X-Request-ID \$request_id;" 5
require_text "$VHOST" 'location = /metrics {'
require_text "$VHOST" 'allow 127.0.0.1;'
require_text "$VHOST" 'allow ::1;'
require_text "$VHOST" 'deny all;'
require_text "$VHOST" 'access_log /var/log/nginx/nuicpp.access.log nuicpp_redacted;'
require_text "$VHOST" 'ssl_certificate /etc/letsencrypt/live/nuicpp.micutu.com/fullchain.pem;'
require_text "$VHOST" 'ssl_certificate_key /etc/letsencrypt/live/nuicpp.micutu.com/privkey.pem;'
require_text "$VHOST" 'listen [::]:443 ssl;'
require_text "$VHOST" 'listen 443 ssl;'
require_text "$VHOST" 'listen [::]:80;'
require_text "$VHOST" 'listen 80;'
require_text "$VHOST" 'proxy_hide_header Origin-Agent-Cluster;'
require_text "$VHOST" 'proxy_hide_header X-Permitted-Cross-Domain-Policies;'
require_text "$VHOST" 'add_header Origin-Agent-Cluster "?1" always;'
require_text "$VHOST" 'add_header X-Permitted-Cross-Domain-Policies "none" always;'
require_text "$VHOST" 'clipboard-read=(), clipboard-write=(self)'
require_text "$VHOST" "font-src 'self';"
require_text "$VHOST" "media-src 'none'; frame-src 'none';"
require_text "$VHOST" "worker-src 'self'; manifest-src 'self';"
require_text "$RATE_LIMITS" "map \$uri \$nuicpp_log_uri {"
require_text "$RATE_LIMITS" 'log_format nuicpp_redacted'

if grep -Eq '^[[:space:]]*add_header Content-Security-Policy .*unsafe-inline' "$VHOST"; then
  echo "nginx config test failed: CSP reintroduced unsafe-inline" >&2
  exit 1
fi
if grep -Eq '^[[:space:]]*add_header Content-Security-Policy .*wasm-unsafe-eval' "$VHOST"; then
  echo "nginx config test failed: server frontend must not grant WebAssembly code generation" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

CERT="$TMP_DIR/fullchain.pem"
KEY="$TMP_DIR/privkey.pem"
RENDERED_VHOST="$TMP_DIR/nuicpp.micutu.com.conf"
TEST_CONFIG="$TMP_DIR/nginx.conf"

# nginx validates certificate material while parsing. Generate an ephemeral
# self-signed pair and replace only host-specific filesystem paths; directives,
# variables, locations, zones and their ordering remain the checked-in candidate.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=nuicpp-config-test.invalid' \
  -keyout "$KEY" -out "$CERT" >/dev/null 2>&1

sed -E \
  -e "s#^([[:space:]]*)access_log .*#\\1access_log $TMP_DIR/access.log nuicpp_redacted;#" \
  -e "s#^([[:space:]]*)ssl_certificate /etc/letsencrypt/live/nuicpp[.]micutu[.]com/fullchain[.]pem;.*#\\1ssl_certificate $CERT;#" \
  -e "s#^([[:space:]]*)ssl_certificate_key /etc/letsencrypt/live/nuicpp[.]micutu[.]com/privkey[.]pem;.*#\\1ssl_certificate_key $KEY;#" \
  -e 's/listen \[::\]:443 ssl;/listen [::]:29443 ssl;/' \
  -e 's/listen 443 ssl;/listen 29443 ssl;/' \
  -e 's/listen \[::\]:80;/listen [::]:29080;/' \
  -e 's/listen 80;/listen 29080;/' \
  -e '/^[[:space:]]*include \/etc\/letsencrypt\/options-ssl-nginx[.]conf;/d' \
  -e '/^[[:space:]]*ssl_dhparam \/etc\/letsencrypt\/ssl-dhparams[.]pem;/d' \
  "$VHOST" > "$RENDERED_VHOST"

{
  printf '%s\n' \
    'worker_processes 1;' \
    "pid $TMP_DIR/nginx.pid;" \
    "error_log $TMP_DIR/error.log;" \
    'events { worker_connections 32; }' \
    'http {' \
    "    access_log $TMP_DIR/default-access.log;" \
    "    map \$remote_addr \$from_cloudflare_origin { default 1; }"
  printf '    include "%s";\n' "$RATE_LIMITS"
  printf '    include "%s";\n' "$RENDERED_VHOST"
  printf '%s\n' '}'
} > "$TEST_CONFIG"

nginx -t -p "$TMP_DIR/" -c "$TEST_CONFIG"
echo "nginx candidate config passed"
