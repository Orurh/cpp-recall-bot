CREATE TABLE analytics_meta (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    enabled_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO analytics_meta(singleton) VALUES (TRUE);

ALTER TABLE attempts ADD COLUMN content_revision TEXT NOT NULL DEFAULT '';
ALTER TABLE sessions ADD COLUMN client_request_id TEXT;
CREATE UNIQUE INDEX sessions_client_request_id ON sessions(user_id, client_request_id)
    WHERE client_request_id IS NOT NULL;

CREATE TABLE learning_events (
    event_key TEXT PRIMARY KEY,
    schema_version INTEGER NOT NULL DEFAULT 1 CHECK (schema_version = 1),
    event_name TEXT NOT NULL CHECK (event_name IN ('bot_start', 'miniapp_open', 'app_active',
        'training_started', 'training_finished', 'session_resumed', 'card_shown', 'answer', 'theory_opened')),
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    session_id BIGINT REFERENCES sessions(id),
    question_id TEXT NOT NULL DEFAULT '',
    content_revision TEXT NOT NULL DEFAULT '',
    attempt_number INTEGER,
    practice_kind TEXT NOT NULL DEFAULT '' CHECK (practice_kind IN ('', 'new', 'review', 'early_review', 'repair')),
    correct BOOLEAN,
    assisted BOOLEAN NOT NULL DEFAULT FALSE,
    outcome TEXT NOT NULL DEFAULT '',
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX learning_events_time_kind ON learning_events(occurred_at, event_name);
CREATE INDEX learning_events_user_time ON learning_events(user_id, occurred_at);
CREATE INDEX learning_events_session ON learning_events(session_id);

CREATE TABLE analytics_visits (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    visit_id UUID NOT NULL,
    opened_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_sequence BIGINT NOT NULL DEFAULT -1,
    PRIMARY KEY (user_id, visit_id)
);
CREATE TABLE analytics_presence (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id),
    visit_id UUID NOT NULL,
    last_seen_at TIMESTAMPTZ NOT NULL,
    session_id BIGINT REFERENCES sessions(id),
    question_id TEXT NOT NULL,
    context TEXT NOT NULL,
    practice_kind TEXT NOT NULL
);
CREATE TABLE activity_samples (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    visit_id UUID NOT NULL,
    sequence BIGINT NOT NULL,
    session_id BIGINT REFERENCES sessions(id),
    question_id TEXT NOT NULL,
    context TEXT NOT NULL,
    practice_kind TEXT NOT NULL,
    active_ms INTEGER NOT NULL CHECK (active_ms BETWEEN 0 AND 15000),
    observed_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY(user_id, visit_id, sequence)
);
CREATE INDEX activity_samples_time ON activity_samples(observed_at);

CREATE FUNCTION analytics_practice_kind(uid BIGINT, qid TEXT, attempt INTEGER) RETURNS TEXT
LANGUAGE sql STABLE AS $function$
    SELECT CASE WHEN attempt > 1 THEN 'repair'
        WHEN NOT EXISTS (SELECT 1 FROM question_progress WHERE user_id = uid AND question_id = qid) THEN 'new'
        WHEN EXISTS (SELECT 1 FROM question_progress WHERE user_id = uid AND question_id = qid AND next_review_at <= now()) THEN 'review'
        ELSE 'early_review' END;
$function$;

CREATE FUNCTION analytics_session_event() RETURNS trigger LANGUAGE plpgsql AS $function$
BEGIN
    IF NEW.user_id <= 0 OR NEW.mode NOT LIKE 'miniapp:%' THEN RETURN NEW; END IF;
    IF TG_OP = 'INSERT' THEN
        INSERT INTO learning_events(event_key, event_name, user_id, session_id)
        VALUES ('session:' || NEW.id || ':start', 'training_started', NEW.user_id, NEW.id)
        ON CONFLICT DO NOTHING;
    ELSIF OLD.finished_at IS NULL AND NEW.finished_at IS NOT NULL THEN
        INSERT INTO learning_events(event_key, event_name, user_id, session_id, outcome)
        VALUES ('session:' || NEW.id || ':finish', 'training_finished', NEW.user_id, NEW.id, NEW.finish_reason)
        ON CONFLICT DO NOTHING;
    END IF;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER analytics_session_event AFTER INSERT OR UPDATE ON sessions
    FOR EACH ROW EXECUTE FUNCTION analytics_session_event();

CREATE FUNCTION analytics_answer_event() RETURNS trigger LANGUAGE plpgsql AS $function$
BEGIN
    IF NEW.user_id <= 0 OR NOT EXISTS (SELECT 1 FROM sessions WHERE id = NEW.session_id AND mode LIKE 'miniapp:%') THEN
        RETURN NEW;
    END IF;
    INSERT INTO learning_events(event_key, event_name, user_id, session_id, question_id, content_revision,
                                attempt_number, practice_kind, correct, assisted)
    VALUES ('answer:' || NEW.id, 'answer', NEW.user_id, NEW.session_id, NEW.question_id, NEW.content_revision,
        NEW.attempt_number, analytics_practice_kind(NEW.user_id, NEW.question_id, NEW.attempt_number), NEW.correct, NEW.assisted)
    ON CONFLICT DO NOTHING;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER analytics_answer_event AFTER INSERT ON attempts
    FOR EACH ROW EXECUTE FUNCTION analytics_answer_event();

CREATE FUNCTION analytics_snapshot_events() RETURNS trigger LANGUAGE plpgsql AS $function$
DECLARE qid TEXT; attempt INTEGER; rev TEXT;
BEGIN
    IF NEW.user_id <= 0 THEN RETURN NEW; END IF;
    IF NEW.state_json #>> '{quiz,answered}' = 'false' THEN
        qid := COALESCE(NEW.state_json #>> '{quiz,primary_queue,0,question_id}',
                        NEW.state_json #>> '{quiz,repair_queue,0,question_id}');
        attempt := (NEW.state_json->>'current_attempt')::INTEGER;
        rev := COALESCE(NEW.state_json->>'current_content_revision', '');
        IF qid IS NOT NULL AND attempt IS NOT NULL THEN
            INSERT INTO learning_events(event_key, event_name, user_id, session_id, question_id,
                                        content_revision, attempt_number, practice_kind)
            VALUES ('shown:' || NEW.session_id || ':' || qid || ':' || attempt, 'card_shown',
                NEW.user_id, NEW.session_id, qid, rev, attempt, analytics_practice_kind(NEW.user_id, qid, attempt))
            ON CONFLICT DO NOTHING;
        END IF;
    END IF;
    FOR qid IN SELECT jsonb_array_elements_text(COALESCE(NEW.state_json->'theory_viewed_question_ids', '[]'::JSONB)) LOOP
        INSERT INTO learning_events(event_key, event_name, user_id, session_id, question_id, content_revision)
        VALUES ('theory:' || NEW.session_id || ':' || qid, 'theory_opened', NEW.user_id, NEW.session_id, qid,
            COALESCE(NEW.state_json #>> '{last_feedback,content_revision}', ''))
        ON CONFLICT DO NOTHING;
    END LOOP;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER analytics_snapshot_events AFTER INSERT OR UPDATE ON active_training_sessions
    FOR EACH ROW EXECUTE FUNCTION analytics_snapshot_events();

CREATE FUNCTION analytics_pulse(uid BIGINT, visit UUID, seq BIGINT, sid BIGINT, qid TEXT, ctx TEXT, kind TEXT)
RETURNS BOOLEAN LANGUAGE plpgsql AS $function$
DECLARE previous analytics_presence%ROWTYPE; last_seq BIGINT; elapsed INTEGER := 0; fresh BOOLEAN;
BEGIN
    IF uid <= 0 THEN RETURN FALSE; END IF;
    PERFORM user_id FROM users WHERE user_id = uid FOR UPDATE;
    INSERT INTO analytics_visits(user_id, visit_id) VALUES (uid, visit) ON CONFLICT DO NOTHING;
    fresh := FOUND;
    SELECT last_sequence INTO last_seq FROM analytics_visits WHERE user_id = uid AND visit_id = visit;
    IF seq <= last_seq THEN RETURN FALSE; END IF;
    IF fresh THEN
        INSERT INTO learning_events(event_key, event_name, user_id)
        VALUES ('open:' || uid || ':' || visit, 'miniapp_open', uid) ON CONFLICT DO NOTHING;
        IF sid IS NOT NULL THEN
            INSERT INTO learning_events(event_key, event_name, user_id, session_id)
            VALUES ('resume:' || sid || ':' || visit, 'session_resumed', uid, sid) ON CONFLICT DO NOTHING;
        END IF;
    END IF;
    IF ctx <> 'hidden' THEN
        INSERT INTO learning_events(event_key, event_name, user_id)
        VALUES ('active:' || uid || ':' || (now() AT TIME ZONE 'UTC')::DATE, 'app_active', uid)
        ON CONFLICT DO NOTHING;
    END IF;
    SELECT * INTO previous FROM analytics_presence WHERE user_id = uid;
    IF FOUND AND previous.visit_id = visit AND previous.context IN ('question', 'feedback', 'theory')
       AND now() - previous.last_seen_at BETWEEN INTERVAL '0 seconds' AND INTERVAL '30 seconds' THEN
        elapsed := LEAST(15000, GREATEST(0, (EXTRACT(EPOCH FROM now() - previous.last_seen_at) * 1000)::INTEGER));
    END IF;
    INSERT INTO activity_samples(user_id, visit_id, sequence, session_id, question_id, context, practice_kind, active_ms)
    VALUES (uid, visit, seq, previous.session_id, COALESCE(previous.question_id, ''),
        COALESCE(previous.context, 'home'), COALESCE(previous.practice_kind, ''), elapsed);
    INSERT INTO analytics_presence(user_id, visit_id, last_seen_at, session_id, question_id, context, practice_kind)
    VALUES (uid, visit, now(), sid, qid, ctx, kind)
    ON CONFLICT (user_id) DO UPDATE SET visit_id = EXCLUDED.visit_id, last_seen_at = EXCLUDED.last_seen_at,
        session_id = EXCLUDED.session_id, question_id = EXCLUDED.question_id, context = EXCLUDED.context,
        practice_kind = EXCLUDED.practice_kind;
    UPDATE analytics_visits SET last_sequence = seq WHERE user_id = uid AND visit_id = visit;
    RETURN TRUE;
END;
$function$;

CREATE FUNCTION beta_statistics(period_days INTEGER) RETURNS JSONB LANGUAGE sql STABLE AS $function$
WITH settings AS (
    SELECT (((now() AT TIME ZONE 'UTC')::DATE - (period_days - 1))::TIMESTAMP AT TIME ZONE 'UTC') AS since,
           (now() AT TIME ZONE 'UTC')::DATE AS today
), events AS (
    SELECT * FROM learning_events WHERE user_id > 0 AND occurred_at >= (SELECT since FROM settings)
), training AS (
    SELECT s.*, CASE
        WHEN s.finish_reason = 'completed' THEN 'completed'
        WHEN s.finish_reason = 'miniapp_stopped' THEN 'stopped'
        WHEN s.finish_reason = 'miniapp_restarted' THEN 'replaced'
        WHEN s.finish_reason IN ('snapshot_invalid', 'snapshot_incompatible') THEN 'technical_failure'
        WHEN s.finish_reason = 'expired' OR (s.finished_at IS NULL AND a.expires_at <= now()) THEN 'expired'
        WHEN s.finished_at IS NULL THEN 'active' ELSE 'other' END AS outcome
    FROM sessions s JOIN events e ON e.session_id = s.id AND e.event_name = 'training_started'
    LEFT JOIN active_training_sessions a ON a.session_id = s.id
), cohort AS (
    SELECT user_id, min((opened_at AT TIME ZONE 'UTC')::DATE) AS first_day FROM analytics_visits WHERE user_id > 0 GROUP BY user_id
), returns AS (
    SELECT c.*, EXISTS (SELECT 1 FROM learning_events e WHERE e.user_id = c.user_id AND e.event_name = 'app_active'
        AND (e.occurred_at AT TIME ZONE 'UTC')::DATE = c.first_day + 1) AS d1,
        EXISTS (SELECT 1 FROM learning_events e WHERE e.user_id = c.user_id AND e.event_name = 'app_active'
        AND (e.occurred_at AT TIME ZONE 'UTC')::DATE = c.first_day + 7) AS d7
    FROM cohort c WHERE c.first_day >= (SELECT (since AT TIME ZONE 'UTC')::DATE FROM settings)
), days AS (
    SELECT day::DATE FROM settings, generate_series(today - (period_days - 1), today, INTERVAL '1 day') AS day
), samples AS (
    SELECT * FROM activity_samples WHERE user_id > 0 AND observed_at >= (SELECT since FROM settings)
), summary AS (
    SELECT jsonb_build_object(
        'bot_users', (SELECT count(DISTINCT user_id) FROM events WHERE event_name = 'bot_start'),
        'opened_users', (SELECT count(DISTINCT user_id) FROM events WHERE event_name = 'app_active'),
        'started_users', count(DISTINCT user_id),
        'completed_users', count(DISTINCT user_id) FILTER (WHERE outcome = 'completed'),
        'sessions_started', count(*), 'completed', count(*) FILTER (WHERE outcome = 'completed'),
        'stopped', count(*) FILTER (WHERE outcome = 'stopped'), 'expired', count(*) FILTER (WHERE outcome = 'expired'),
        'replaced', count(*) FILTER (WHERE outcome = 'replaced'), 'active', count(*) FILTER (WHERE outcome = 'active'),
        'technical_failure', count(*) FILTER (WHERE outcome = 'technical_failure'),
        'completion_rate', round(100.0 * count(*) FILTER (WHERE outcome = 'completed') / NULLIF(count(*), 0), 1),
        'median_wall_seconds', percentile_cont(0.5) WITHIN GROUP (ORDER BY EXTRACT(EPOCH FROM finished_at - started_at)) FILTER (WHERE outcome = 'completed'),
        'resumes', (SELECT count(*) FROM events WHERE event_name = 'session_resumed'),
        'attempts', (SELECT count(*) FROM events WHERE event_name = 'answer'),
        'errors', (SELECT count(*) FROM events WHERE event_name = 'answer' AND NOT correct),
        'corrections', (SELECT count(*) FROM events WHERE event_name = 'answer' AND correct AND attempt_number > 1),
        'theory_opened', (SELECT count(*) FROM events WHERE event_name = 'theory_opened'),
        'active_seconds', (SELECT COALESCE(sum(active_ms), 0) / 1000 FROM samples),
        'new_cards', (SELECT count(*) FROM events WHERE event_name = 'answer' AND practice_kind = 'new'),
        'review_cards', (SELECT count(*) FROM events WHERE event_name = 'answer' AND practice_kind = 'review'),
        'early_review_cards', (SELECT count(*) FROM events WHERE event_name = 'answer' AND practice_kind = 'early_review'),
        'repair_attempts', (SELECT count(*) FROM events WHERE event_name = 'answer' AND practice_kind = 'repair')
    ) AS value FROM training
)
SELECT jsonb_build_object(
    'days', period_days, 'timezone', 'UTC',
    'tracking_since', (SELECT to_char(enabled_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') FROM analytics_meta),
    'summary', (SELECT value FROM summary),
    'returns', jsonb_build_object(
        'd1_eligible', (SELECT count(*) FROM returns WHERE first_day < (SELECT today - 1 FROM settings)),
        'd1_returned', (SELECT count(*) FROM returns WHERE first_day < (SELECT today - 1 FROM settings) AND d1),
        'd7_eligible', (SELECT count(*) FROM returns WHERE first_day < (SELECT today - 7 FROM settings)),
        'd7_returned', (SELECT count(*) FROM returns WHERE first_day < (SELECT today - 7 FROM settings) AND d7)),
    'activity', (SELECT COALESCE(jsonb_agg(jsonb_build_object(
        'day', day, 'visitors', (SELECT count(DISTINCT user_id) FROM events WHERE event_name = 'app_active' AND (occurred_at AT TIME ZONE 'UTC')::DATE = day),
        'learners', (SELECT count(DISTINCT user_id) FROM events WHERE event_name = 'answer' AND (occurred_at AT TIME ZONE 'UTC')::DATE = day),
        'started', (SELECT count(*) FROM events WHERE event_name = 'training_started' AND (occurred_at AT TIME ZONE 'UTC')::DATE = day),
        'completed', (SELECT count(*) FROM events WHERE event_name = 'training_finished' AND outcome = 'completed' AND (occurred_at AT TIME ZONE 'UTC')::DATE = day)
    ) ORDER BY day DESC), '[]'::JSONB) FROM days),
    'time_by_kind', (SELECT COALESCE(jsonb_agg(row), '[]'::JSONB) FROM (
        SELECT context, practice_kind, sum(active_ms) / 1000 AS seconds FROM samples
        WHERE active_ms > 0 GROUP BY context, practice_kind ORDER BY context, practice_kind
    ) row)
);
$function$;
