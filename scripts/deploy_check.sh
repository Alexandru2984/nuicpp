#!/usr/bin/env bash
set -euo pipefail

APP_HOST="${APP_HOST:-127.0.0.1}"
APP_PORT="${APP_PORT:-18081}"

echo "Liveness:"
curl -fsS "http://${APP_HOST}:${APP_PORT}/live"
echo
echo "Readiness:"
curl -fsS "http://${APP_HOST}:${APP_PORT}/ready"
echo
echo "Listening sockets:"
ss -ltnp | grep ":${APP_PORT}" || true
echo "Systemd:"
systemctl --no-pager --full status nuigraph-studio.service || true
