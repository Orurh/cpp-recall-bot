CREATE TABLE IF NOT EXISTS skill_evidence (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    skill_id TEXT NOT NULL CHECK (skill_id <> ''),
    question_family_id TEXT NOT NULL CHECK (question_family_id <> ''),
    question_id TEXT NOT NULL CHECK (question_id <> ''),
    evidence_type TEXT NOT NULL
        CHECK (evidence_type IN ('recognition', 'application')),
    transfer BOOLEAN NOT NULL DEFAULT false,
    successful BOOLEAN NOT NULL DEFAULT false,
    clean_success BOOLEAN NOT NULL DEFAULT false,
    delayed_new_application BOOLEAN NOT NULL DEFAULT false,
    first_recorded_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_recorded_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, skill_id, question_family_id)
);

CREATE INDEX IF NOT EXISTS idx_skill_evidence_user_skill
    ON skill_evidence(user_id, skill_id);

CREATE TABLE IF NOT EXISTS user_skill_state (
    user_id BIGINT NOT NULL REFERENCES users(user_id),
    skill_id TEXT NOT NULL CHECK (skill_id <> ''),
    status TEXT NOT NULL
        CHECK (status IN ('encountered', 'applying', 'demonstrated', 'retained', 'needs_recovery')),
    successful_family_count INTEGER NOT NULL DEFAULT 0
        CHECK (successful_family_count >= 0),
    clean_family_count INTEGER NOT NULL DEFAULT 0
        CHECK (clean_family_count >= 0),
    application_family_count INTEGER NOT NULL DEFAULT 0
        CHECK (application_family_count >= 0),
    transfer_family_count INTEGER NOT NULL DEFAULT 0
        CHECK (transfer_family_count >= 0),
    demonstrated_at TIMESTAMPTZ,
    retained_at TIMESTAMPTZ,
    needs_recovery_since TIMESTAMPTZ,
    last_evidence_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, skill_id)
);

CREATE INDEX IF NOT EXISTS idx_user_skill_state_user_status
    ON user_skill_state(user_id, status);
