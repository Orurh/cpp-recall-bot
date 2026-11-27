-- Friendship is independent from learning progress. No Telegram contact import.
CREATE TABLE friendship_invites (
    token TEXT PRIMARY KEY CHECK (token ~ '^[0-9a-f]{32}$'),
    owner_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    expires_at TIMESTAMPTZ NOT NULL DEFAULT (clock_timestamp() + interval '24 hours'),
    closed_at TIMESTAMPTZ,
    CHECK (expires_at > created_at)
);
CREATE UNIQUE INDEX friendship_one_open_invite ON friendship_invites(owner_id) WHERE closed_at IS NULL;
CREATE INDEX friendship_invites_owner_time ON friendship_invites(owner_id, created_at DESC);

CREATE TABLE friendships (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    low_user BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    high_user BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK (low_user < high_user),
    UNIQUE (low_user, high_user)
);
CREATE INDEX friendships_high_user ON friendships(high_user);

CREATE TABLE friendship_blocks (
    owner_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    blocked_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    CHECK (owner_id <> blocked_id),
    PRIMARY KEY (owner_id, blocked_id)
);
