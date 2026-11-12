-- Run manually on a local copy with a read-only role. No raw answers are exported.
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY;
SET LOCAL statement_timeout = '10s';
WITH observations AS (
    SELECT a.answered_at AS at, 'answer:' || a.id AS event_id,
        jsonb_build_object(
            'id', 'answer:' || a.id, 'direction', 'cpp', 'user_id', a.user_id,
            'question_id', a.question_id, 'revision', a.content_revision,
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
    WHERE a.user_id > 0 AND a.answered_at <= now()
    UNION ALL
    SELECT e.occurred_at, 'help:' || e.event_key,
        jsonb_build_object(
            'id', 'help:' || e.event_key, 'direction', 'cpp', 'user_id', e.user_id,
            'question_id', e.question_id, 'revision', e.content_revision,
            'skill_id', r.content_snapshot->>'skill_id',
            'family_id', r.content_snapshot->>'question_family_id',
            'evidence_type', r.content_snapshot->>'evidence_type',
            'occurred_at', to_char(e.occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
            'event', 'help', 'attempt_number', NULL, 'correct', NULL,
            'assisted', true, 'help_known', true
        )
    FROM learning_events e
    JOIN sessions s ON s.id = e.session_id AND s.user_id = e.user_id
    LEFT JOIN question_content_revisions r
        ON r.question_id = e.question_id AND r.content_revision = e.content_revision
    WHERE e.user_id > 0 AND e.occurred_at <= now()
      AND e.event_name IN ('reference_opened', 'theory_opened')
), bounded AS (
    SELECT * FROM observations ORDER BY at, event_id LIMIT 50001
)
SELECT jsonb_build_object(
    'schema', 'cpp-recall-retrieval-input/v1',
    'observed_until', to_char(now() AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
    'tracking_since', (SELECT to_char(min(enabled_at) AT TIME ZONE 'UTC',
        'YYYY-MM-DD"T"HH24:MI:SS.US"Z"') FROM analytics_meta),
    'complete', count(*) <= 50000,
    'read_only', current_setting('transaction_read_only') = 'on',
    'events', COALESCE(jsonb_agg(value ORDER BY at, event_id), '[]'::jsonb)
) FROM bounded;
ROLLBACK;
