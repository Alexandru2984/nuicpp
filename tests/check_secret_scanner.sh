#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

clean_fixture="$TMP_DIR/clean.env"
# This is an intentional unexpanded placeholder fixture.
# shellcheck disable=SC2016
printf '%s\n' \
  'SESSION_SIGNING_SECRET=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc' \
  'CSRF_SECRET=${GENERATED_AT_RUNTIME}' \
  'DATABASE_URL=postgresql://nuigraph_test:test-password@127.0.0.1/nuigraph_studio_test' \
  > "$clean_fixture"
python3 "$ROOT_DIR/scripts/scan_secrets.py" --files "$clean_fixture" >/dev/null

secret_fixture="$TMP_DIR/leak.env"
secret_value="$(openssl rand -hex 32)"
printf 'SESSION_SIGNING_SECRET=%s\n' "$secret_value" > "$secret_fixture"
report="$TMP_DIR/report.txt"
if python3 "$ROOT_DIR/scripts/scan_secrets.py" --files "$secret_fixture" >"$report" 2>&1; then
  echo "secret scanner accepted a high-entropy credential" >&2
  exit 1
fi
grep -Fq '[generic-high-entropy-credential]' "$report"
if grep -Fq "$secret_value" "$report"; then
  echo "secret scanner copied a credential into its report" >&2
  exit 1
fi

key_fixture="$TMP_DIR/private-key.pem"
printf '%s%s\n' '-----BEGIN ' 'PRIVATE KEY-----' > "$key_fixture"
if python3 "$ROOT_DIR/scripts/scan_secrets.py" --files "$key_fixture" >/dev/null 2>&1; then
  echo "secret scanner accepted a private-key marker" >&2
  exit 1
fi

link_fixture="$TMP_DIR/source-link"
ln -s "$clean_fixture" "$link_fixture"
if python3 "$ROOT_DIR/scripts/scan_secrets.py" --files "$link_fixture" >/dev/null 2>&1; then
  echo "secret scanner followed a source symlink" >&2
  exit 1
fi

python3 "$ROOT_DIR/scripts/scan_secrets.py" >/dev/null
echo "secret scanner tests passed"
