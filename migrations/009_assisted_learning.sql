ALTER TABLE attempts
    DROP CONSTRAINT IF EXISTS attempts_attempt_number_check;

ALTER TABLE attempts
    ADD CONSTRAINT attempts_attempt_number_check
        CHECK (attempt_number BETWEEN 1 AND 1000);

ALTER TABLE attempts
    ADD COLUMN IF NOT EXISTS assisted BOOLEAN NOT NULL DEFAULT false;

CREATE INDEX IF NOT EXISTS idx_attempts_user_assisted_time
    ON attempts(user_id, answered_at)
    WHERE assisted;
