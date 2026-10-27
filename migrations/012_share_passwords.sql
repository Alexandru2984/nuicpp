-- Optional second factor for bearer share links. Only the derived PBKDF2 value
-- is stored; the monotonically increasing version invalidates old unlock cookies.
ALTER TABLE diagrams
    ADD COLUMN share_password_hash TEXT,
    ADD COLUMN share_password_version BIGINT NOT NULL DEFAULT 0;

ALTER TABLE diagrams
    ADD CONSTRAINT diagrams_share_password_hash_format
        CHECK (
            share_password_hash IS NULL OR
            share_password_hash ~ '^pbkdf2_sha256\$[0-9]{6,9}\$[0-9a-f]{32}\$[0-9a-f]{64}$'
        ),
    ADD CONSTRAINT diagrams_share_password_version_nonnegative
        CHECK (share_password_version >= 0);

COMMENT ON COLUMN diagrams.share_password_hash IS
    'Optional PBKDF2-SHA256 verifier for an active bearer share; never returned by the API.';
COMMENT ON COLUMN diagrams.share_password_version IS
    'Incremented on password changes/removal to revoke stateless share-unlock cookies.';
