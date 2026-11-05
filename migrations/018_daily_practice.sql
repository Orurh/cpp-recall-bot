-- Additive daily identity; snapshots, attempts and completion receipts remain
-- the sources of truth for execution, practice and completion respectively.
CREATE TABLE user_daily_settings (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id) ON DELETE CASCADE,
    timezone TEXT NOT NULL CHECK (length(timezone) BETWEEN 1 AND 128)
);

CREATE TABLE daily_practice_assignments (
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    local_day DATE NOT NULL,
    timezone TEXT NOT NULL CHECK (length(timezone) BETWEEN 1 AND 128),
    session_id BIGINT NOT NULL UNIQUE REFERENCES sessions(id) ON DELETE CASCADE,
    question_id TEXT NOT NULL CHECK (length(question_id) BETWEEN 1 AND 256),
    selection_reason TEXT NOT NULL CHECK (length(selection_reason) BETWEEN 1 AND 64),
    content_version TEXT NOT NULL CHECK (length(content_version) BETWEEN 1 AND 128),
    assigned_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (user_id, local_day)
);
