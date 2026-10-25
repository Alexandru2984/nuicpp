-- Retry-safe creation records are scoped to a purpose-specific HMAC of the
-- authenticated principal. Raw session identifiers and bearer share slugs are
-- never persisted. The diagram foreign key bounds retained records and removes
-- them when their result is deleted; the application also prunes expired rows.
CREATE TABLE IF NOT EXISTS public.diagram_idempotency_keys (
    principal_key CHAR(64) NOT NULL
        CHECK (principal_key ~ '^[0-9a-f]{64}$'),
    operation VARCHAR(24) NOT NULL
        CHECK (operation IN ('create', 'template', 'fork', 'duplicate', 'import')),
    key_hash CHAR(64) NOT NULL
        CHECK (key_hash ~ '^[0-9a-f]{64}$'),
    request_hash CHAR(64) NOT NULL
        CHECK (request_hash ~ '^[0-9a-f]{64}$'),
    diagram_id BIGINT NOT NULL REFERENCES public.diagrams(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT statement_timestamp(),
    expires_at TIMESTAMPTZ NOT NULL DEFAULT statement_timestamp() + interval '24 hours',
    PRIMARY KEY (principal_key, operation, key_hash),
    CHECK (expires_at > created_at)
);

CREATE INDEX IF NOT EXISTS idx_diagram_idempotency_keys_expires_at
    ON public.diagram_idempotency_keys (expires_at);

COMMENT ON TABLE public.diagram_idempotency_keys IS
    'Bounded retry-safety records for diagram-creating API operations';
