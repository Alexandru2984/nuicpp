-- Application abuse limits must survive process restarts and be shared by
-- every instance. The key is an HMAC-SHA256 digest produced by the app; raw IP
-- addresses and session identifiers are deliberately never stored here.
CREATE TABLE IF NOT EXISTS rate_limit_buckets (
    bucket_key VARCHAR(64) PRIMARY KEY
        CHECK (bucket_key ~ '^[0-9a-f]{64}$'),
    theoretical_arrival_at TIMESTAMPTZ NOT NULL,
    expires_at TIMESTAMPTZ NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_rate_limit_buckets_expires_at
    ON rate_limit_buckets (expires_at);
