-- Administrator sessions are individually revocable. The browser holds a
-- 256-bit token; PostgreSQL stores only its SHA-256 digest and bounded metadata.
CREATE TABLE admin_sessions (
    session_id CHAR(32) PRIMARY KEY CHECK (session_id ~ '^[0-9a-f]{32}$'),
    token_hash CHAR(64) NOT NULL UNIQUE CHECK (token_hash ~ '^[0-9a-f]{64}$'),
    auth_state_hash CHAR(64) NOT NULL CHECK (auth_state_hash ~ '^[0-9a-f]{64}$'),
    device_label VARCHAR(64) NOT NULL CHECK (length(device_label) BETWEEN 1 AND 64),
    created_at TIMESTAMPTZ NOT NULL DEFAULT statement_timestamp(),
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT statement_timestamp(),
    expires_at TIMESTAMPTZ NOT NULL,
    revoked_at TIMESTAMPTZ,
    CHECK (expires_at > created_at),
    CHECK (revoked_at IS NULL OR revoked_at >= created_at)
);

CREATE INDEX idx_admin_sessions_active
    ON admin_sessions (expires_at DESC)
    WHERE revoked_at IS NULL;

COMMENT ON TABLE admin_sessions IS
    'Bounded, individually revocable administrator sessions; contains no raw cookie token, IP address or user-agent.';
