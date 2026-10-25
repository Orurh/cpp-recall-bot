-- Derived only from observed events since 011. Never backfill learning or XP.
-- observed_at is a server-side test seam; the HTTP API always uses its default.
CREATE FUNCTION learning_statistics(period_days INTEGER, observed_at TIMESTAMPTZ DEFAULT now())
RETURNS JSONB LANGUAGE sql STABLE AS $function$
WITH settings AS (
    SELECT (observed_at AT TIME ZONE 'UTC')::DATE AS today,
        ((((observed_at AT TIME ZONE 'UTC')::DATE - (period_days - 1))::TIMESTAMP)
            AT TIME ZONE 'UTC') AS since
), answers AS NOT MATERIALIZED (
    SELECT user_id, session_id, occurred_at FROM learning_events
    WHERE user_id > 0 AND event_name = 'answer' AND occurred_at < observed_at
), first_answers AS (
    SELECT user_id, min(occurred_at) AS first_at FROM answers GROUP BY user_id
), learners AS (
    SELECT user_id, (first_at AT TIME ZONE 'UTC')::DATE AS first_day FROM first_answers
    WHERE first_at >= (SELECT since FROM settings)
), return_windows AS (
    SELECT l.*, w.name, w.last_day, l.first_day + w.last_day < s.today AS eligible,
        EXISTS (SELECT 1 FROM answers a WHERE a.user_id = l.user_id
            AND a.occurred_at >= ((l.first_day + w.first_day)::TIMESTAMP AT TIME ZONE 'UTC')
            AND a.occurred_at < ((l.first_day + w.last_day + 1)::TIMESTAMP AT TIME ZONE 'UTC')) AS returned
    FROM learners l CROSS JOIN settings s
    CROSS JOIN (VALUES ('d1', 1, 1), ('d7_9', 7, 9)) AS w(name, first_day, last_day)
), first_starts AS (
    SELECT DISTINCT ON (user_id) user_id, session_id, occurred_at AS started_at
    FROM learning_events WHERE user_id > 0 AND event_name = 'training_started'
        AND session_id IS NOT NULL AND occurred_at < observed_at
    ORDER BY user_id, occurred_at, session_id
), first_training AS (
    SELECT f.*, EXISTS (SELECT 1 FROM answers a
        WHERE a.user_id = f.user_id AND a.session_id = f.session_id
            AND a.occurred_at >= f.started_at) AS answered,
        EXISTS (SELECT 1 FROM learning_events e WHERE e.event_name = 'training_finished'
            AND e.user_id = f.user_id AND e.session_id = f.session_id AND e.outcome = 'completed'
            AND e.occurred_at >= f.started_at AND e.occurred_at < observed_at) AS completed
    FROM first_starts f WHERE f.started_at >= (SELECT since FROM settings)
)
SELECT jsonb_build_object(
    'rules_version', 1, 'timezone', 'UTC',
    'cohort_since', (SELECT (since AT TIME ZONE 'UTC')::DATE FROM settings),
    'observed_at', to_char(observed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"'),
    'activated_users', (SELECT count(*) FROM learners),
    'first_training', (SELECT jsonb_build_object(
        'started_users', count(*),
        'answered_users', count(*) FILTER (WHERE answered),
        'completed_users', count(*) FILTER (WHERE completed),
        'completion_rate', round(100.0 * count(*) FILTER (WHERE completed) / NULLIF(count(*), 0), 1)
    ) FROM first_training),
    'returns', (SELECT jsonb_object_agg(w.name, (SELECT jsonb_build_object(
        'observed', count(*),
        'eligible', count(*) FILTER (WHERE eligible),
        'pending', count(*) FILTER (WHERE NOT eligible),
        'returned', count(*) FILTER (WHERE eligible AND returned),
        'rate', round(100.0 * count(*) FILTER (WHERE eligible AND returned)
            / NULLIF(count(*) FILTER (WHERE eligible), 0), 1)
    ) FROM return_windows r WHERE r.name = w.name)) FROM (VALUES ('d1'), ('d7_9')) AS w(name))
);
$function$;
