CREATE TABLE IF NOT EXISTS user_reward_unlock (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    reward_id TEXT NOT NULL CHECK (reward_id <> ''),
    unlocked_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    announced_at TIMESTAMPTZ,
    PRIMARY KEY (user_id, reward_id)
);

CREATE INDEX IF NOT EXISTS idx_user_reward_unlock_user_time
    ON user_reward_unlock(user_id, unlocked_at);

CREATE TABLE IF NOT EXISTS user_reward_settings (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id),
    profile_frame TEXT NOT NULL DEFAULT 'none',
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS training_presets (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 1 AND 5),
    name TEXT NOT NULL CHECK (name <> ''),
    modes TEXT NOT NULL CHECK (modes <> ''),
    topics TEXT NOT NULL DEFAULT '',
    level INTEGER NOT NULL CHECK (level BETWEEN 1 AND 10),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, slot)
);

-- Старые пользователи получают уже заработанные действующие награды.
INSERT INTO user_reward_unlock(user_id, reward_id, unlocked_at, announced_at)
SELECT
    users.user_id,
    reward.reward_id,
    now(),
    NULL
FROM users
CROSS JOIN (VALUES
    ('profile_frames', 150),
    ('second_preset', 400)
) AS reward(reward_id, threshold)
WHERE COALESCE((
    SELECT SUM(delta)
    FROM xp_ledger
    WHERE xp_ledger.user_id = users.user_id
), 0) >= reward.threshold
ON CONFLICT (user_id, reward_id) DO NOTHING;

-- Совместимость с базами, созданными старой версией модели освоения.
ALTER TABLE skill_evidence
    ALTER COLUMN delayed_new_application SET DEFAULT false;
