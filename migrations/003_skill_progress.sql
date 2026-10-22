CREATE TABLE IF NOT EXISTS skill_progress (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    skill_id TEXT NOT NULL CHECK (skill_id <> ''),
    question_id TEXT NOT NULL CHECK (question_id <> ''),
    completed_count INTEGER NOT NULL DEFAULT 0 CHECK (completed_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0 CHECK (correct_count >= 0),
    failed_count INTEGER NOT NULL DEFAULT 0 CHECK (failed_count >= 0),
    first_try_correct_count INTEGER NOT NULL DEFAULT 0 CHECK (first_try_correct_count >= 0),
    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, skill_id, question_id)
);

CREATE INDEX IF NOT EXISTS idx_skill_progress_user_last
    ON skill_progress(user_id, last_answered_at);
