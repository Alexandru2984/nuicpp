-- Existing enabled links keep their current no-expiry behavior. Owners can add
-- a deadline explicitly after the application release understands this field.
ALTER TABLE diagrams
    ADD COLUMN IF NOT EXISTS share_expires_at TIMESTAMPTZ;

COMMENT ON COLUMN diagrams.share_expires_at IS
    'Database-clock deadline for an enabled bearer link; NULL means no automatic expiry';
