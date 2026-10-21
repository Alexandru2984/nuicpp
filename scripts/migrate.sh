#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "migration environment file not found: $ENV_FILE" >&2
  exit 1
fi

# Environment files are data, not shell programs. In particular this script is
# run through sudo during deploy, so sourcing the file would turn any shell
# syntax in it into root command execution.
DATABASE_URL="$(sed -n 's/^DATABASE_URL=//p' "$ENV_FILE" | tail -n 1)"
DATABASE_URL="${DATABASE_URL%\"}"
DATABASE_URL="${DATABASE_URL#\"}"
DATABASE_URL="${DATABASE_URL%\'}"
DATABASE_URL="${DATABASE_URL#\'}"
if [[ -z "$DATABASE_URL" ]]; then
  echo "DATABASE_URL is missing from $ENV_FILE" >&2
  exit 1
fi

for migration in "$ROOT_DIR"/migrations/*.sql; do
  psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -f "$migration"
done
