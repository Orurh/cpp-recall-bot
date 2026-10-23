CREATE TABLE IF NOT EXISTS active_training_sessions (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id) ON DELETE CASCADE,
    session_id BIGINT NOT NULL UNIQUE REFERENCES sessions(id) ON DELETE CASCADE,
    state_json JSONB NOT NULL,
    state_version BIGINT NOT NULL DEFAULT 1 CHECK (state_version > 0),
    content_version TEXT NOT NULL CHECK (content_version <> ''),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ NOT NULL DEFAULT now() + INTERVAL '24 hours'
);

CREATE INDEX IF NOT EXISTS active_training_sessions_expires_at_idx
    ON active_training_sessions(expires_at);
