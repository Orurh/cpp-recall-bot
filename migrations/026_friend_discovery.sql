-- Explicit opt-in; no imported contact list is persisted.
CREATE TABLE friend_discovery (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id) ON DELETE CASCADE,
    enabled BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE TABLE friend_discovery_requests (
    user_id BIGINT PRIMARY KEY REFERENCES users(user_id) ON DELETE CASCADE,
    request_id INTEGER NOT NULL CHECK (request_id > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    consumed BOOLEAN NOT NULL DEFAULT FALSE
);
