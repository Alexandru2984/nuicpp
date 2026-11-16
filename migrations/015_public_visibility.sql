-- Public discovery is an explicit state layered on top of an active unlisted
-- link. Existing private and unlisted diagrams remain unchanged.
ALTER TABLE diagrams
    ADD COLUMN publicly_listed BOOLEAN NOT NULL DEFAULT false;

ALTER TABLE diagrams
    ADD CONSTRAINT diagrams_public_requires_sharing
    CHECK (NOT publicly_listed OR sharing_enabled);

ALTER TABLE diagrams
    ADD CONSTRAINT diagrams_public_cannot_use_password
    CHECK (NOT publicly_listed OR share_password_hash IS NULL);

CREATE INDEX idx_diagrams_public_catalog
    ON diagrams (updated_at DESC, id DESC)
    WHERE publicly_listed AND sharing_enabled AND deleted_at IS NULL
      AND share_password_hash IS NULL;

COMMENT ON COLUMN diagrams.publicly_listed IS
    'Explicit owner opt-in for search-indexable catalog and /p page visibility; false remains private or unlisted.';
