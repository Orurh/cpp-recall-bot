-- Up to three ordered medals, each belonging to the same account.
CREATE TABLE IF NOT EXISTS user_achievement_showcase (
    user_id BIGINT NOT NULL,
    position SMALLINT NOT NULL CHECK (position BETWEEN 1 AND 3),
    achievement_id TEXT NOT NULL,
    PRIMARY KEY (user_id, position),
    UNIQUE (user_id, achievement_id),
    FOREIGN KEY (user_id, achievement_id)
        REFERENCES user_achievement_unlock(user_id, achievement_id) ON DELETE CASCADE
);
