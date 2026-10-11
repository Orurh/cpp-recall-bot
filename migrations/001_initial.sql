CREATE TABLE IF NOT EXISTS users (
    user_id BIGINT PRIMARY KEY,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    target_date DATE,
    adaptive_level INTEGER NOT NULL DEFAULT 3
        CHECK (adaptive_level BETWEEN 1 AND 10)
);

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS adaptive_level INTEGER NOT NULL DEFAULT 3;

CREATE TABLE IF NOT EXISTS sessions (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at TIMESTAMPTZ,
    mode TEXT NOT NULL,
    finish_reason TEXT
);

ALTER TABLE sessions
    ADD COLUMN IF NOT EXISTS finish_reason TEXT;

CREATE INDEX IF NOT EXISTS idx_sessions_user_started
    ON sessions(user_id, started_at);

CREATE TABLE IF NOT EXISTS attempts (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    session_id BIGINT NOT NULL REFERENCES sessions(id),
    question_id TEXT NOT NULL,
    answer TEXT NOT NULL,
    correct BOOLEAN NOT NULL,
    attempt_number INTEGER NOT NULL CHECK (attempt_number BETWEEN 1 AND 3),
    answered_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_attempts_user_question_time
    ON attempts(user_id, question_id, answered_at);

CREATE INDEX IF NOT EXISTS idx_attempts_user_time
    ON attempts(user_id, answered_at);

CREATE UNIQUE INDEX IF NOT EXISTS idx_attempts_unique_submission
    ON attempts(session_id, question_id, attempt_number);

CREATE TABLE IF NOT EXISTS question_progress (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    question_id TEXT NOT NULL,
    completed_count INTEGER NOT NULL DEFAULT 0 CHECK (completed_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0 CHECK (correct_count >= 0),
    failed_count INTEGER NOT NULL DEFAULT 0 CHECK (failed_count >= 0),
    first_try_correct_count INTEGER NOT NULL DEFAULT 0 CHECK (first_try_correct_count >= 0),
    review_stage INTEGER NOT NULL DEFAULT 0 CHECK (review_stage BETWEEN 0 AND 5),
    consecutive_correct INTEGER NOT NULL DEFAULT 0 CHECK (consecutive_correct >= 0),
    next_review_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, question_id)
);

ALTER TABLE question_progress
    ADD COLUMN IF NOT EXISTS review_stage INTEGER NOT NULL DEFAULT 0
        CHECK (review_stage BETWEEN 0 AND 5);

ALTER TABLE question_progress
    ADD COLUMN IF NOT EXISTS consecutive_correct INTEGER NOT NULL DEFAULT 0
        CHECK (consecutive_correct >= 0);

ALTER TABLE question_progress
    ADD COLUMN IF NOT EXISTS next_review_at TIMESTAMPTZ NOT NULL DEFAULT now();

CREATE INDEX IF NOT EXISTS idx_question_progress_user_last
    ON question_progress(user_id, last_answered_at);

CREATE INDEX IF NOT EXISTS idx_question_progress_user_review
    ON question_progress(user_id, next_review_at);

CREATE TABLE IF NOT EXISTS topic_progress (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    topic TEXT NOT NULL,
    adaptive_level INTEGER NOT NULL DEFAULT 3 CHECK (adaptive_level BETWEEN 1 AND 10),
    completed_count INTEGER NOT NULL DEFAULT 0 CHECK (completed_count >= 0),
    correct_count INTEGER NOT NULL DEFAULT 0 CHECK (correct_count >= 0),
    failed_count INTEGER NOT NULL DEFAULT 0 CHECK (failed_count >= 0),
    first_try_correct_count INTEGER NOT NULL DEFAULT 0 CHECK (first_try_correct_count >= 0),
    last_answered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, topic)
);

CREATE INDEX IF NOT EXISTS idx_topic_progress_user_last
    ON topic_progress(user_id, last_answered_at);
