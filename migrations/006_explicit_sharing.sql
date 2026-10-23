-- Existing share URLs were already live bearer capabilities, so preserve them
-- during the upgrade. New rows use the private-by-default column default.
ALTER TABLE diagrams
    ADD COLUMN IF NOT EXISTS sharing_enabled BOOLEAN;

UPDATE diagrams
SET sharing_enabled = true
WHERE sharing_enabled IS NULL;

ALTER TABLE diagrams
    ALTER COLUMN sharing_enabled SET DEFAULT false,
    ALTER COLUMN sharing_enabled SET NOT NULL;
