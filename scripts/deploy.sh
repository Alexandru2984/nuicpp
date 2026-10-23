#!/usr/bin/env bash
# Build, test, migrate and atomically publish a root-owned NuiGraph release.
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

DEPLOY_ROOT=/opt/nuigraph-studio
RELEASES="$DEPLOY_ROOT/releases"
CURRENT="$DEPLOY_ROOT/current"
APP_CONFIG=/etc/nuigraph-studio/app.env
MIGRATION_CONFIG=/etc/nuigraph-studio/migrate.env
TEST_CONFIG=/etc/nuigraph-studio/test.env
DEPLOY_COMMAND_PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
STAGED_LINK=""
PREVIOUS=""

cleanup() {
  if [[ -n "$STAGED_LINK" ]] && sudo test -L "$STAGED_LINK"; then
    sudo unlink "$STAGED_LINK"
  fi
}
trap cleanup EXIT

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
  echo "refusing to deploy a dirty or untracked working tree" >&2
  git status --short
  exit 1
fi

for required in "$APP_CONFIG" "$MIGRATION_CONFIG" "$TEST_CONFIG"; do
  if ! sudo test -f "$required"; then
    echo "missing deployment configuration: $required" >&2
    exit 1
  fi
done

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j "$(nproc)"
npm ci --prefix nui-frontend --ignore-scripts
npm audit --prefix nui-frontend --audit-level=high
nui-frontend/node_modules/.bin/playwright install chromium
sudo env -i PATH="$DEPLOY_COMMAND_PATH" PROJECT_ROOT="$ROOT_DIR" \
  ./build/nuigraph-studio check-config "$APP_CONFIG"
sudo env -i PATH="$DEPLOY_COMMAND_PATH" ./build/nuigraph-studio check-config "$TEST_CONFIG"

# Live suites mutate data. Migrate and use only the dedicated test database;
# REQUIRE turns missing/unavailable prerequisites into a deploy failure.
sudo env -i PATH="$DEPLOY_COMMAND_PATH" ENV_FILE="$TEST_CONFIG" ./scripts/migrate.sh
sudo env -i PATH="$DEPLOY_COMMAND_PATH" ENV_FILE="$TEST_CONFIG" NUIGRAPH_REQUIRE_LIVE_TESTS=1 \
  ctest --test-dir build --output-on-failure
node scripts/generate_sbom.mjs build/sbom

sudo env -i PATH="$DEPLOY_COMMAND_PATH" ENV_FILE="$MIGRATION_CONFIG" ./scripts/migrate.sh
sudo -u postgres psql -v ON_ERROR_STOP=1 -d nuigraph_studio \
  -v app_user=nuigraph_app -v migration_owner=nuigraph_user \
  -f deploy/postgres/runtime_privileges.sql

release_id="$(git rev-parse --short=12 HEAD)-$(date -u +'%Y%m%dT%H%M%SZ')"
release="$RELEASES/$release_id"
sudo install -d -o root -g root -m 0755 "$DEPLOY_ROOT" "$RELEASES" "$release"
sudo install -o root -g root -m 0555 build/nuigraph-studio "$release/nuigraph-studio"
sudo install -d -o root -g root -m 0555 "$release/sbom"
sudo install -o root -g root -m 0444 \
  build/sbom/nuigraph-native.cdx.json \
  build/sbom/nuigraph-npm.cdx.json \
  "$release/sbom/"
sudo cp -a public "$release/public"
sudo chown -R root:root "$release/public"
sudo find "$release/public" -type d -exec chmod 0555 {} +
sudo find "$release/public" -type f -exec chmod 0444 {} +

if sudo test -L "$CURRENT"; then
  PREVIOUS="$(sudo readlink -f "$CURRENT")"
fi
STAGED_LINK="$DEPLOY_ROOT/.current-$release_id"
sudo ln -s "$release" "$STAGED_LINK"
sudo mv -Tf "$STAGED_LINK" "$CURRENT"
STAGED_LINK=""

healthy=0
if sudo systemctl restart nuigraph-studio.service; then
  for _ in $(seq 1 40); do
    if [[ "$(curl -sS -o /dev/null -w '%{http_code}' http://127.0.0.1:18081/ready || true)" == 200 ]]; then
      healthy=1
      break
    fi
    sleep 0.25
  done
fi

if [[ "$healthy" == 1 ]] && ! curl -fsS https://nuicpp.micutu.com/ready >/dev/null; then
  healthy=0
fi
if [[ "$healthy" == 1 ]] && ! ./scripts/deploy_check.sh; then
  healthy=0
fi

if [[ "$healthy" != 1 ]]; then
  echo "deployment failed; restoring the previous release" >&2
  if [[ -n "$PREVIOUS" ]]; then
    STAGED_LINK="$DEPLOY_ROOT/.rollback-$release_id"
    sudo ln -s "$PREVIOUS" "$STAGED_LINK"
    sudo mv -Tf "$STAGED_LINK" "$CURRENT"
    STAGED_LINK=""
    sudo systemctl restart nuigraph-studio.service || true
  else
    sudo systemctl stop nuigraph-studio.service || true
  fi
  sudo journalctl -u nuigraph-studio.service -n 30 --no-pager
  exit 1
fi

echo "release $release_id deployed and healthy"
