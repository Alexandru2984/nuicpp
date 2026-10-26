-- Soft deletion is an explicit lifecycle state. Trashed diagrams remain in
-- row and logical-byte quotas until their owner permanently deletes them.
ALTER TABLE public.diagrams
    ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMPTZ;

CREATE INDEX IF NOT EXISTS idx_diagrams_active_updated
    ON public.diagrams (updated_at DESC, id DESC)
    WHERE deleted_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_diagrams_trash_deleted
    ON public.diagrams (deleted_at DESC, id DESC)
    WHERE deleted_at IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_diagrams_trash_owner_deleted
    ON public.diagrams (owner_token_hash, deleted_at DESC, id DESC)
    WHERE deleted_at IS NOT NULL;

COMMENT ON COLUMN public.diagrams.deleted_at IS
    'Database-clock trash timestamp; content remains private and quota-accounted until permanent deletion';
