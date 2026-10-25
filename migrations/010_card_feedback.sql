CREATE TABLE question_content_revisions (
    question_id TEXT NOT NULL,
    content_revision TEXT NOT NULL CHECK (content_revision ~ '^[0-9a-f]{64}$'),
    content_snapshot JSONB NOT NULL,
    archived_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (question_id, content_revision)
);

CREATE TABLE card_feedback (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    session_id BIGINT NOT NULL REFERENCES sessions(id),
    request_id TEXT NOT NULL CHECK (request_id ~ '^[0-9a-f-]{36}$'),
    question_id TEXT NOT NULL CHECK (question_id <> ''),
    public_id TEXT NOT NULL CHECK (public_id ~ '^CR-[0-9]{6}$' AND public_id <> 'CR-000000'),
    content_revision TEXT NOT NULL CHECK (content_revision ~ '^[0-9a-f]{64}$'),
    content_snapshot JSONB NOT NULL CHECK (
        jsonb_typeof(content_snapshot) = 'object'
        AND content_snapshot->>'id' IS NOT DISTINCT FROM question_id
        AND content_snapshot->>'public_id' IS NOT DISTINCT FROM public_id
    ),
    phase TEXT NOT NULL CHECK (phase IN ('question', 'feedback')),
    category TEXT NOT NULL CHECK (category IN ('technical_error', 'unclear_wording', 'answer_or_source', 'other')),
    comment TEXT NOT NULL CHECK (octet_length(comment) BETWEEN 1 AND 8000),
    status TEXT NOT NULL DEFAULT 'new' CHECK (status IN ('new', 'in_review', 'resolved', 'rejected')),
    decision TEXT NOT NULL DEFAULT '' CHECK (octet_length(decision) <= 8000),
    source TEXT NOT NULL DEFAULT '' CHECK (octet_length(source) <= 2048),
    state_version BIGINT NOT NULL DEFAULT 1 CHECK (state_version > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (user_id, request_id),
    CHECK (status NOT IN ('resolved', 'rejected') OR (decision <> '' AND source ~ '^https://[^[:space:]]+$'))
);

CREATE INDEX idx_card_feedback_queue ON card_feedback(status, id DESC);
CREATE INDEX idx_card_feedback_user_time ON card_feedback(user_id, created_at);

CREATE TABLE card_feedback_review_history (
    id BIGSERIAL PRIMARY KEY,
    feedback_id BIGINT NOT NULL REFERENCES card_feedback(id),
    reviewer_id BIGINT NOT NULL,
    state_version BIGINT NOT NULL,
    status TEXT NOT NULL,
    decision TEXT NOT NULL,
    source TEXT NOT NULL,
    reviewed_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (feedback_id, state_version)
);
