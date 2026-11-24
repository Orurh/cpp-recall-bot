-- Only future clean completions populate promotion evidence. Existing levels,
-- unlocked maxima, scalar counters, skill evidence and attempts are untouched.
ALTER TABLE users
    ADD COLUMN IF NOT EXISTS adaptive_promotion JSONB NOT NULL DEFAULT '{}'::JSONB
        CHECK (jsonb_typeof(adaptive_promotion) = 'object');

ALTER TABLE topic_progress
    ADD COLUMN IF NOT EXISTS adaptive_promotion JSONB NOT NULL DEFAULT '{}'::JSONB
        CHECK (jsonb_typeof(adaptive_promotion) = 'object');
