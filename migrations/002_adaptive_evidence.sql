ALTER TABLE users
    ADD COLUMN IF NOT EXISTS adaptive_evidence INTEGER NOT NULL DEFAULT 0
        CHECK (adaptive_evidence BETWEEN -3 AND 3);

ALTER TABLE topic_progress
    ADD COLUMN IF NOT EXISTS adaptive_evidence INTEGER NOT NULL DEFAULT 0
        CHECK (adaptive_evidence BETWEEN -3 AND 3);
