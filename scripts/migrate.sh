#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -a
source "$ROOT_DIR/.env"
set +a

psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f "$ROOT_DIR/migrations/001_init.sql"
