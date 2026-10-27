-- Persist the greatest accepted administrator TOTP counter so a successful
-- six-digit code cannot be replayed inside its validity window or across an
-- application restart. The singleton update is the atomic compare-and-swap.
CREATE TABLE admin_totp_state (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    last_accepted_counter BIGINT NOT NULL DEFAULT -1
        CHECK (last_accepted_counter >= -1),
    updated_at TIMESTAMPTZ
);

INSERT INTO admin_totp_state (singleton, last_accepted_counter)
VALUES (TRUE, -1);

COMMENT ON TABLE admin_totp_state IS
    'Singleton replay barrier for the optional administrator RFC 6238 factor; contains no TOTP secret or code.';
