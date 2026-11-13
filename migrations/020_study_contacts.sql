-- Server offers, not proof of rendering or reading. No historical backfill.
CREATE TABLE study_contact_tracking (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    started_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);
INSERT INTO study_contact_tracking(singleton) VALUES (TRUE);

CREATE TABLE study_contacts (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    session_id BIGINT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    question_id TEXT NOT NULL CHECK (question_id <> ''),
    -- NULL means legacy/unknown, never substituted with today's revision.
    -- A legacy snapshot may name an unavailable archive: retain its hash, not
    -- today's content. Consumers must join the archive before using metadata.
    question_content_revision TEXT CHECK (length(question_content_revision) = 64),
    attempt_number INTEGER NOT NULL CHECK (attempt_number BETWEEN 1 AND 1000),
    kind TEXT NOT NULL CHECK (kind IN (
        'question_delivered', 'feedback_delivered', 'reference_opened', 'theory_opened', 'answer_revealed')),
    reference_id TEXT,
    reference_content_revision TEXT,
    offered_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    FOREIGN KEY (reference_id, reference_content_revision)
        REFERENCES reference_content_revisions(reference_id, content_revision),
    CHECK ((kind = 'reference_opened' AND reference_id IS NOT NULL AND reference_content_revision IS NOT NULL)
        OR (kind <> 'reference_opened' AND reference_id IS NULL AND reference_content_revision IS NULL))
);
CREATE INDEX study_contacts_user_time_idx ON study_contacts(user_id, offered_at, id);
CREATE INDEX study_contacts_encounter_idx
    ON study_contacts(session_id, question_id, question_content_revision, attempt_number, id);
