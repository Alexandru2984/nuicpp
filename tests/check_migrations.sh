#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$ROOT_DIR/.env}"
BINARY="$ROOT_DIR/build/nuigraph-studio"

# shellcheck source=tests/live_test_helpers.sh
source "$ROOT_DIR/tests/live_test_helpers.sh"
require_isolated_live_test_database "migration ledger"

expected_count=0
for migration in "$ROOT_DIR"/migrations/*.sql; do
  migration_name="$(basename "$migration")"
  if [[ ! "$migration_name" =~ ^[0-9]{3}_[a-z0-9_]+\.sql$ ]]; then
    echo "migration ledger failed: invalid filename $migration_name" >&2
    exit 1
  fi
  expected_checksum="$(sha256sum "$migration" | cut -d' ' -f1)"
  actual_checksum="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -Atqc \
    "SELECT checksum FROM nuigraph_meta.schema_migrations WHERE filename = '$migration_name'")"
  if [[ "$actual_checksum" != "$expected_checksum" ]]; then
    echo "migration ledger failed: checksum mismatch for $migration_name" >&2
    exit 1
  fi
  expected_count=$((expected_count + 1))
done

actual_count="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 -Atqc \
  'SELECT count(*) FROM nuigraph_meta.schema_migrations')"
if [[ "$actual_count" != "$expected_count" ]]; then
  echo "migration ledger failed: expected $expected_count rows, found $actual_count" >&2
  exit 1
fi

echo "migration ledger passed"
