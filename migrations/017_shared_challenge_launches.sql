-- One signed Telegram launch, not a message sent or a click outside the app.
-- No raw initData, sender, chat ID, answer or referral identifier is retained.
CREATE TABLE shared_challenge_launches (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    challenge_id TEXT NOT NULL CHECK (length(challenge_id) BETWEEN 1 AND 64),
    auth_date BIGINT NOT NULL CHECK (auth_date > 0),
    outcome TEXT NOT NULL CHECK (outcome IN ('ready', 'locked', 'unavailable')),
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, challenge_id, auth_date)
);
CREATE INDEX shared_challenge_launches_time ON shared_challenge_launches(occurred_at);
