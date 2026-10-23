CREATE TABLE IF NOT EXISTS xp_ledger (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    event_type TEXT NOT NULL CHECK (event_type <> ''),
    source_id TEXT NOT NULL CHECK (source_id <> ''),
    cycle_id TEXT NOT NULL CHECK (cycle_id <> ''),
    delta INTEGER NOT NULL CHECK (delta > 0),
    rules_version INTEGER NOT NULL CHECK (rules_version > 0),
    awarded_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (user_id, event_type, source_id, cycle_id)
);

CREATE INDEX IF NOT EXISTS idx_xp_ledger_user_awarded
    ON xp_ledger(user_id, awarded_at DESC);
