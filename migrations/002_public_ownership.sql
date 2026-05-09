ALTER TABLE diagrams
ADD COLUMN IF NOT EXISTS owner_token_hash VARCHAR(128);

CREATE INDEX IF NOT EXISTS idx_diagrams_owner_token_hash ON diagrams(owner_token_hash);
