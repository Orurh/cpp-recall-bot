ALTER TABLE learning_events DROP CONSTRAINT learning_events_event_name_check;
ALTER TABLE learning_events ADD CHECK (event_name IN (
    'bot_start', 'miniapp_open', 'app_active', 'training_started', 'training_finished',
    'session_resumed', 'card_shown', 'answer', 'theory_opened', 'reference_opened'
));

-- One immutable event per card and training, even when no answer follows.
CREATE FUNCTION analytics_reference_events() RETURNS trigger LANGUAGE plpgsql AS $function$
DECLARE qid TEXT;
BEGIN
    IF NEW.user_id <= 0 THEN RETURN NEW; END IF;
    FOR qid IN SELECT jsonb_array_elements_text(
        COALESCE(NEW.state_json->'reference_viewed_question_ids', '[]'::JSONB)) LOOP
        INSERT INTO learning_events(event_key, event_name, user_id, session_id, question_id,
                                    content_revision, assisted)
        VALUES ('reference:' || NEW.session_id || ':' || qid, 'reference_opened',
                NEW.user_id, NEW.session_id, qid,
                COALESCE(NEW.state_json->>'current_content_revision', ''), true)
        ON CONFLICT DO NOTHING;
    END LOOP;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER analytics_reference_events AFTER INSERT OR UPDATE ON active_training_sessions
    FOR EACH ROW EXECUTE FUNCTION analytics_reference_events();
