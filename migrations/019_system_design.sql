-- Independent educational model. No writes/triggers into C++ learning tables.
CREATE TABLE system_design_module_progress (
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    module_id TEXT NOT NULL,
    content_revision TEXT NOT NULL,
    completed_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, module_id, content_revision)
);

CREATE TABLE system_design_sessions (
    id BIGSERIAL PRIMARY KEY,
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    case_id TEXT NOT NULL,
    content_revision TEXT NOT NULL,
    start_request_id TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'completed')),
    current_stage INTEGER NOT NULL DEFAULT 0 CHECK (current_stage >= 0),
    state_version BIGINT NOT NULL DEFAULT 1 CHECK (state_version > 0),
    state_json JSONB NOT NULL CHECK (jsonb_typeof(state_json) = 'object'),
    started_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at TIMESTAMPTZ,
    UNIQUE (user_id, start_request_id),
    UNIQUE (user_id, id),
    CHECK ((status = 'completed') = (completed_at IS NOT NULL))
);
CREATE UNIQUE INDEX system_design_active_case
    ON system_design_sessions(user_id, case_id) WHERE status = 'active';
CREATE INDEX system_design_history ON system_design_sessions(user_id, started_at DESC);

CREATE TABLE system_design_commands (
    user_id BIGINT NOT NULL,
    session_id BIGINT NOT NULL,
    request_id TEXT NOT NULL,
    request_json JSONB NOT NULL,
    applied_version BIGINT NOT NULL CHECK (applied_version > 0),
    PRIMARY KEY (session_id, request_id),
    FOREIGN KEY (user_id, session_id)
        REFERENCES system_design_sessions(user_id, id) ON DELETE CASCADE
);

CREATE TABLE system_design_results (
    user_id BIGINT NOT NULL,
    session_id BIGINT PRIMARY KEY,
    review_json JSONB NOT NULL CHECK (jsonb_typeof(review_json) = 'object'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    FOREIGN KEY (user_id, session_id)
        REFERENCES system_design_sessions(user_id, id) ON DELETE CASCADE
);
