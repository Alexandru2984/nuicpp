-- A database-owned schedule prevents a multi-instance deployment from purging
-- one batch per process. The application advances this row in the same
-- transaction as each bounded purge.
CREATE TABLE IF NOT EXISTS public.maintenance_jobs (
    job_name VARCHAR(64) PRIMARY KEY
        CHECK (job_name ~ '^[a-z][a-z0-9_]{0,63}$'),
    next_run_at TIMESTAMPTZ NOT NULL DEFAULT '-infinity',
    last_success_at TIMESTAMPTZ,
    last_deleted_count INTEGER NOT NULL DEFAULT 0
        CHECK (last_deleted_count >= 0)
);

INSERT INTO public.maintenance_jobs (job_name)
VALUES ('trash_purge')
ON CONFLICT (job_name) DO NOTHING;

COMMENT ON TABLE public.maintenance_jobs IS
    'Database-clock coordination for bounded application maintenance jobs';
