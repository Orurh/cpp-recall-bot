-- Keep the old one-row-per-user table and its primary key: an automatic image
-- rollback must still be able to read/update the current training. A separate
-- table retains unfinished snapshots, including a mirror of the current one.
ALTER TABLE active_training_sessions ALTER COLUMN expires_at SET DEFAULT 'infinity'::timestamptz;
UPDATE active_training_sessions SET expires_at = 'infinity';

CREATE TABLE saved_training_sessions (
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    session_id BIGINT PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,
    state_json JSONB NOT NULL,
    state_version BIGINT NOT NULL CHECK (state_version > 0),
    content_version TEXT NOT NULL CHECK (content_version <> ''),
    updated_at TIMESTAMPTZ NOT NULL,
    expires_at TIMESTAMPTZ NOT NULL DEFAULT 'infinity'
);
CREATE INDEX saved_training_sessions_user_idx ON saved_training_sessions(user_id);

INSERT INTO saved_training_sessions
    (user_id, session_id, state_json, state_version, content_version, updated_at, expires_at)
SELECT user_id, session_id, state_json, state_version, content_version, updated_at, 'infinity'
FROM active_training_sessions;

CREATE FUNCTION preserve_training_snapshot() RETURNS trigger LANGUAGE plpgsql AS $function$
BEGIN
    INSERT INTO saved_training_sessions
        (user_id, session_id, state_json, state_version, content_version, updated_at, expires_at)
    VALUES (NEW.user_id, NEW.session_id, NEW.state_json, NEW.state_version,
            NEW.content_version, NEW.updated_at, 'infinity')
    ON CONFLICT (session_id) DO UPDATE SET
        state_json = EXCLUDED.state_json, state_version = EXCLUDED.state_version,
        content_version = EXCLUDED.content_version, updated_at = EXCLUDED.updated_at,
        expires_at = EXCLUDED.expires_at;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER preserve_training_snapshot AFTER INSERT OR UPDATE ON active_training_sessions
    FOR EACH ROW EXECUTE FUNCTION preserve_training_snapshot();

-- The previous binary implicitly closes *all* unfinished trainings on start.
-- Suppress that legacy replacement, but allow explicit stop/completion/expiry.
-- The old DELETE of the active row is safe: the mirror above already exists.
CREATE FUNCTION preserve_training_on_legacy_restart() RETURNS trigger LANGUAGE plpgsql AS $function$
BEGIN
    IF OLD.finished_at IS NULL AND NEW.finished_at IS NOT NULL
       AND NEW.finish_reason = 'miniapp_restarted'
       AND EXISTS (SELECT 1 FROM saved_training_sessions WHERE session_id = OLD.id) THEN
        RETURN NULL;
    END IF;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER preserve_training_on_legacy_restart BEFORE UPDATE ON sessions
    FOR EACH ROW EXECUTE FUNCTION preserve_training_on_legacy_restart();

CREATE FUNCTION remove_finished_training_snapshot() RETURNS trigger LANGUAGE plpgsql AS $function$
BEGIN
    IF NEW.finished_at IS NOT NULL THEN
        DELETE FROM saved_training_sessions WHERE session_id = NEW.id;
    END IF;
    RETURN NEW;
END;
$function$;
CREATE TRIGGER remove_finished_training_snapshot AFTER UPDATE ON sessions
    FOR EACH ROW EXECUTE FUNCTION remove_finished_training_snapshot();
