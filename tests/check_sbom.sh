#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NUI_CONFIG="$ROOT_DIR/nui-frontend/_cmake/dependencies/nui.cmake"
EXPECTED_NUI_COMMIT="547f34d694acfae7abf802733273113fcacddc98"
TMP_DIR="$(mktemp -d)"

cleanup() {
  find "$TMP_DIR" -type f -delete
  find "$TMP_DIR" -type l -delete
  rmdir "$TMP_DIR"
}
trap cleanup EXIT

if ! grep -Eq "GIT_TAG[[:space:]]+${EXPECTED_NUI_COMMIT}([[:space:]]|$)" "$NUI_CONFIG"; then
  echo "Nui dependency is not pinned to the reviewed immutable commit" >&2
  exit 1
fi
if grep -Eq 'GIT_TAG[[:space:]]+v?[0-9]+\.[0-9]+' "$NUI_CONFIG"; then
  echo "Nui dependency still uses a mutable version tag" >&2
  exit 1
fi

node "$ROOT_DIR/scripts/generate_sbom.mjs" "$TMP_DIR"
node "$ROOT_DIR/tests/validate_sbom.mjs" \
  "$TMP_DIR/nuigraph-native.cdx.json" \
  "$TMP_DIR/nuigraph-npm.cdx.json"

ln -s "$TMP_DIR" "$TMP_DIR/output-link"
if node "$ROOT_DIR/scripts/generate_sbom.mjs" "$TMP_DIR/output-link" >/dev/null 2>&1; then
  echo "SBOM generator followed a symlinked output directory" >&2
  exit 1
fi
unlink "$TMP_DIR/output-link"

echo "SBOM generation and dependency pins passed"
