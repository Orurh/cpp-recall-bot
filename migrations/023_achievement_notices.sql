-- Explicit acknowledgement; reads must not consume notices.
ALTER TABLE user_achievement_unlock ADD COLUMN IF NOT EXISTS seen_at TIMESTAMPTZ;
