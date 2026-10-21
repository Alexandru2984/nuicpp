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

# Keep migration metadata outside public so the runtime role cannot read or
# alter the ledger through its application-table grants.
psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -c "
  BEGIN;
  SELECT pg_advisory_xact_lock(hashtextextended('nuigraph:schema-migrations', 0));
  CREATE SCHEMA IF NOT EXISTS nuigraph_meta;
  REVOKE ALL ON SCHEMA nuigraph_meta FROM PUBLIC;
  CREATE TABLE IF NOT EXISTS nuigraph_meta.schema_migrations (
    filename TEXT PRIMARY KEY,
    checksum TEXT NOT NULL CHECK (checksum ~ '^[0-9a-f]{64}$'),
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
  );
  COMMIT;
"

for migration in "$ROOT_DIR"/migrations/*.sql; do
  migration_name="$(basename "$migration")"
  if [[ ! "$migration_name" =~ ^[0-9]{3}_[a-z0-9_]+\.sql$ ]]; then
    echo "invalid migration filename: $migration_name" >&2
    exit 1
  fi
  migration_checksum="$(sha256sum "$migration" | cut -d' ' -f1)"
  psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 \
    -v migration_name="$migration_name" \
    -v migration_checksum="$migration_checksum" \
    -v migration_path="$migration" \
    -f "$ROOT_DIR/scripts/apply_migration.sql"
done
