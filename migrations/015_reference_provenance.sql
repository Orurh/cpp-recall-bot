-- Additive: old binaries still understand the assistance marker in snapshots.
-- Never invent article identities for legacy reference_opened events.
CREATE TABLE reference_content_revisions (
    reference_id TEXT NOT NULL CHECK (octet_length(reference_id) BETWEEN 1 AND 64),
    content_revision TEXT NOT NULL CHECK (content_revision ~ '^[0-9a-f]{64}$'),
    content_snapshot JSONB NOT NULL CHECK (
        jsonb_typeof(content_snapshot) = 'object'
        AND content_snapshot->>'id' IS NOT DISTINCT FROM reference_id
    ),
    archived_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (reference_id, content_revision)
);

CREATE TABLE reference_views (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    session_id BIGINT NOT NULL REFERENCES sessions(id),
    question_id TEXT NOT NULL,
    question_content_revision TEXT NOT NULL,
    reference_id TEXT NOT NULL,
    reference_content_revision TEXT NOT NULL,
    attempt_number INTEGER NOT NULL CHECK (attempt_number BETWEEN 1 AND 1000),
    opened_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    FOREIGN KEY (question_id, question_content_revision)
        REFERENCES question_content_revisions(question_id, content_revision),
    FOREIGN KEY (reference_id, reference_content_revision)
        REFERENCES reference_content_revisions(reference_id, content_revision),
    UNIQUE (session_id, question_id, question_content_revision, reference_id, reference_content_revision)
);
CREATE INDEX reference_views_owner_card ON reference_views(user_id, session_id, question_id, question_content_revision);

-- Freeze the visible openings when feedback is submitted. Later help must not
-- silently change the evidence attached to an earlier report or its retry.
ALTER TABLE card_feedback ADD COLUMN reference_context JSONB NOT NULL DEFAULT '[]'::JSONB
    CHECK (jsonb_typeof(reference_context) = 'array');
