-- Run this as the database owner or a PostgreSQL superuser after migrations.
-- It keeps the application role on DML-only runtime privileges and removes
-- object creation rights that are only needed for schema migrations.

\set ON_ERROR_STOP on

\if :{?app_user}
\else
  \set app_user nuigraph_app
\endif

\if :{?migration_owner}
\else
  \set migration_owner nuigraph_user
\endif

SELECT (:'app_user' <> :'migration_owner') AS roles_are_distinct \gset
\if :roles_are_distinct
\else
  \echo 'runtime and migration roles must be distinct'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

SELECT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = :'app_user') AS app_role_exists \gset
\if :app_role_exists
\else
  \echo 'runtime role does not exist'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

SELECT (
  rolcanlogin AND NOT rolsuper AND NOT rolcreaterole AND NOT rolcreatedb AND
  NOT rolreplication AND NOT rolbypassrls AND NOT EXISTS (
    SELECT 1 FROM pg_auth_members WHERE member = pg_roles.oid
  )
) AS app_role_safe
FROM pg_roles WHERE rolname = :'app_user' \gset
\if :app_role_safe
\else
  \echo 'runtime role has unsafe attributes or memberships'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

SELECT (pg_get_userbyid(datdba) = :'migration_owner') AS database_owner_ok
FROM pg_database WHERE datname = current_database() \gset
\if :database_owner_ok
\else
  \echo 'migration role must own the database'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

SELECT (pg_get_userbyid(nspowner) = :'migration_owner') AS schema_owner_ok
FROM pg_namespace WHERE nspname = 'public' \gset
\if :schema_owner_ok
\else
  \echo 'migration role must own the public schema'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

SELECT EXISTS (
  SELECT 1 FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE n.nspname = 'public' AND c.relkind IN ('r', 'S')
    AND pg_get_userbyid(c.relowner) <> :'migration_owner'
) AS unexpected_object_owner \gset
\if :unexpected_object_owner
  \echo 'migration role must own every table and sequence'
  SELECT 1 / 0 AS privilege_contract_violation;
\endif

REVOKE ALL ON DATABASE nuigraph_studio FROM :"app_user";
REVOKE ALL ON SCHEMA public FROM :"app_user";
REVOKE CREATE ON SCHEMA public FROM PUBLIC;
REVOKE ALL ON ALL TABLES IN SCHEMA public FROM :"app_user";
REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM :"app_user";

GRANT CONNECT ON DATABASE nuigraph_studio TO :"app_user";
GRANT USAGE ON SCHEMA public TO :"app_user";

GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO :"app_user";
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO :"app_user";

ALTER DEFAULT PRIVILEGES FOR ROLE :"migration_owner" IN SCHEMA public
GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO :"app_user";

ALTER DEFAULT PRIVILEGES FOR ROLE :"migration_owner" IN SCHEMA public
GRANT USAGE, SELECT ON SEQUENCES TO :"app_user";
