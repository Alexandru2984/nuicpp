\set ON_ERROR_STOP on

BEGIN;

-- Recheck while holding a database-wide transaction lock. Two deployers may
-- both observe a pending file before one acquires the lock; only the first is
-- then allowed to apply it.
SELECT pg_advisory_xact_lock(hashtextextended('nuigraph:schema-migrations', 0));

SELECT
  NOT EXISTS (
    SELECT 1
    FROM nuigraph_meta.schema_migrations
    WHERE filename = :'migration_name'
  ) AS migration_pending,
  COALESCE((
    SELECT checksum = :'migration_checksum'
    FROM nuigraph_meta.schema_migrations
    WHERE filename = :'migration_name'
  ), true) AS checksum_matches
\gset

\if :checksum_matches
\else
  \echo 'checksum mismatch for applied migration' :migration_name
  SELECT 1 / 0 AS migration_checksum_mismatch;
\endif

\if :migration_pending
  \echo 'applying migration' :migration_name
  \i :migration_path
  INSERT INTO nuigraph_meta.schema_migrations (filename, checksum)
  VALUES (:'migration_name', :'migration_checksum');
\else
  \echo 'migration already applied' :migration_name
\endif

COMMIT;
