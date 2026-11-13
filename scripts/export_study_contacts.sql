-- Run on a disposable/local copy with a read-only role. No raw answers or content.
-- Contacts describe server offers, not proof that material was read.
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY;
SET LOCAL statement_timeout = '10s';
WITH observations AS (
    SELECT a.answered_at AS at, 'answer:' || a.id AS event_id,
        jsonb_build_object(
            'id', 'answer:' || a.id, 'direction', 'cpp', 'user_id', a.user_id,
            'session_id', a.session_id, 'question_id', a.question_id,
            'revision', a.content_revision,
            'skill_id', r.content_snapshot->>'skill_id',
            'family_id', r.content_snapshot->>'question_family_id',
            'evidence_type', r.content_snapshot->>'evidence_type',
            'occurred_at', to_char(a.answered_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
            'event', 'answer', 'attempt_number', a.attempt_number,
            'correct', a.correct, 'assisted', a.assisted,
            'help_known', EXISTS (
                SELECT 1 FROM learning_events e
                WHERE e.event_key = 'answer:' || a.id AND e.event_name = 'answer'
                  AND e.user_id = a.user_id AND e.session_id = a.session_id
                  AND e.question_id = a.question_id AND e.content_revision = a.content_revision
                  AND e.assisted = a.assisted
            )
        ) AS value
    FROM attempts a
    JOIN sessions s ON s.id = a.session_id AND s.user_id = a.user_id
    LEFT JOIN question_content_revisions r
        ON r.question_id = a.question_id AND r.content_revision = a.content_revision
    WHERE a.user_id > 0 AND a.session_id > 0 AND a.answered_at <= now()
    UNION ALL
    SELECT c.offered_at, 'contact:' || c.id,
        jsonb_build_object(
            'id', 'contact:' || c.id, 'direction', 'cpp', 'user_id', c.user_id,
            'session_id', c.session_id, 'question_id', c.question_id,
            'revision', c.question_content_revision,
            'skill_id', r.content_snapshot->>'skill_id',
            'family_id', r.content_snapshot->>'question_family_id',
            'evidence_type', r.content_snapshot->>'evidence_type',
            'occurred_at', to_char(c.offered_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
            'event', 'contact', 'attempt_number', c.attempt_number, 'kind', c.kind,
            'reference_id', c.reference_id, 'reference_revision', c.reference_content_revision
        )
    FROM study_contacts c
    JOIN sessions s ON s.id = c.session_id AND s.user_id = c.user_id
    LEFT JOIN question_content_revisions r
        ON r.question_id = c.question_id AND r.content_revision = c.question_content_revision
    WHERE c.user_id > 0 AND c.session_id > 0 AND c.offered_at <= now()
    UNION ALL
    -- Legacy first-help events are retained even when newer contacts also exist.
    -- Their missing attempt is deliberately unknown to the encounter classifier.
    SELECT e.occurred_at, 'legacy-help:' || e.event_key,
        jsonb_build_object(
            'id', 'legacy-help:' || e.event_key, 'direction', 'cpp', 'user_id', e.user_id,
            'session_id', e.session_id, 'question_id', e.question_id,
            'revision', e.content_revision,
            'skill_id', r.content_snapshot->>'skill_id',
            'family_id', r.content_snapshot->>'question_family_id',
            'evidence_type', r.content_snapshot->>'evidence_type',
            'occurred_at', to_char(e.occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
            'event', 'help', 'attempt_number', NULL,
            'correct', NULL, 'assisted', true, 'help_known', true
        )
    FROM learning_events e
    JOIN sessions s ON s.id = e.session_id AND s.user_id = e.user_id
    LEFT JOIN question_content_revisions r
        ON r.question_id = e.question_id AND r.content_revision = e.content_revision
    WHERE e.user_id > 0 AND e.session_id > 0 AND e.occurred_at <= now()
      AND e.event_name IN ('reference_opened', 'theory_opened')
), bounded AS (
    SELECT * FROM observations ORDER BY at, event_id LIMIT 50001
)
SELECT jsonb_build_object(
    'schema', 'cpp-recall-study-input/v1',
    'observed_until', to_char(now() AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
    'tracking_since', (SELECT to_char(min(enabled_at) AT TIME ZONE 'UTC',
        'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') FROM analytics_meta),
    'contacts_since', (SELECT to_char(min(started_at) AT TIME ZONE 'UTC',
        'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') FROM study_contact_tracking),
    'complete', count(*) <= 50000,
    'read_only', current_setting('transaction_read_only') = 'on',
    'events', COALESCE(jsonb_agg(value ORDER BY at, event_id), '[]'::jsonb)
) FROM bounded;
ROLLBACK;
