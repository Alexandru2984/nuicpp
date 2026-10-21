#!/usr/bin/env bash

# Shared fail-closed setup for suites that start the application and mutate a
# PostgreSQL database. Callers must define ROOT_DIR, ENV_FILE and BINARY first.

live_test_env_value() {
  local key="$1"
  local value
  value="$(sed -n "s/^${key}=//p" "$ENV_FILE" | tail -n 1)"
  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"
  printf '%s' "$value"
}

live_test_skip_or_fail() {
  local suite="$1"
  local reason="$2"

  if [[ "${NUIGRAPH_REQUIRE_LIVE_TESTS:-0}" == "1" ]]; then
    echo "${suite} failed: ${reason}" >&2
    exit 1
  fi
  echo "${suite} skipped: ${reason}"
  # CMake registers 77 as SKIP_RETURN_CODE, so this is reported as Not Run
  # rather than as a misleading successful test.
  exit 77
}

require_isolated_live_test_database() {
  local suite="$1"
  local app_env
  local connection_identity
  local database_name
  local database_role

  case "${NUIGRAPH_REQUIRE_LIVE_TESTS:-0}" in
    0|1) ;;
    *)
      echo "${suite} failed: NUIGRAPH_REQUIRE_LIVE_TESTS must be 0 or 1" >&2
      exit 1
      ;;
  esac

  if [[ ! -x "$BINARY" ]]; then
    live_test_skip_or_fail "$suite" "missing build/nuigraph-studio"
  fi
  if [[ ! -f "$ENV_FILE" ]]; then
    live_test_skip_or_fail "$suite" "missing configured environment file"
  fi

  app_env="$(live_test_env_value APP_ENV)"
  DATABASE_URL="$(live_test_env_value DATABASE_URL)"
  if [[ "$app_env" != "test" ]]; then
    echo "${suite} refused: APP_ENV must be exactly 'test'" >&2
    exit 1
  fi
  if [[ -z "$DATABASE_URL" ]]; then
    echo "${suite} refused: DATABASE_URL is missing" >&2
    exit 1
  fi

  if ! connection_identity="$(psql --dbname="$DATABASE_URL" -X -v ON_ERROR_STOP=1 \
      -AqtF '|' -c "SELECT current_database(), current_user" 2>/dev/null)"; then
    live_test_skip_or_fail "$suite" "configured test database is unavailable"
  fi
  IFS='|' read -r database_name database_role <<<"$connection_identity"
  if [[ "$database_name" != *_test ]]; then
    echo "${suite} refused: database '$database_name' does not end in _test" >&2
    exit 1
  fi
  if [[ "$database_role" != *_test ]]; then
    echo "${suite} refused: database role '$database_role' does not end in _test" >&2
    exit 1
  fi
}
