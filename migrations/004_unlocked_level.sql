ALTER TABLE users
    ADD COLUMN IF NOT EXISTS unlocked_level INTEGER NOT NULL DEFAULT 3
        CHECK (unlocked_level BETWEEN 1 AND 10);

UPDATE users
SET unlocked_level = GREATEST(unlocked_level, adaptive_level);
