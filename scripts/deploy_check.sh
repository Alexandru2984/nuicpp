#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -a
source "$ROOT_DIR/.env"
set +a

echo "Health:"
curl -fsS "http://${APP_HOST}:${APP_PORT}/health"
echo
echo "Listening sockets:"
ss -ltnp | grep ":${APP_PORT}" || true
echo "Systemd:"
systemctl --no-pager --full status nuigraph-studio.service || true
