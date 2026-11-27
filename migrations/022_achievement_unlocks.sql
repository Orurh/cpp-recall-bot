-- Permanent, server-observed achievement unlocks. Do not infer historical dates.
CREATE TABLE IF NOT EXISTS user_achievement_unlock (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    achievement_id TEXT NOT NULL CHECK (achievement_id <> ''),
    rules_version INTEGER NOT NULL CHECK (rules_version > 0),
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, achievement_id)
);
