-- Run this as the database owner or a PostgreSQL superuser after migrations.
-- It keeps the application role on DML-only runtime privileges and removes
-- object creation rights that are only needed for schema migrations.

\set app_user nuigraph_user

REVOKE CREATE ON DATABASE nuigraph_studio FROM :app_user;
REVOKE CREATE ON SCHEMA public FROM :app_user;

GRANT CONNECT ON DATABASE nuigraph_studio TO :app_user;
GRANT USAGE ON SCHEMA public TO :app_user;

GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO :app_user;
GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO :app_user;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
GRANT SELECT, INSERT, UPDATE, DELETE ON TABLES TO :app_user;

ALTER DEFAULT PRIVILEGES IN SCHEMA public
GRANT USAGE, SELECT ON SEQUENCES TO :app_user;
