-- Additive: old images still read/write the existing session tables. Do not
-- invent receipts for historical completions whose exact result was not saved.
CREATE TABLE training_completion_receipts (
    session_id BIGINT PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,
    user_id BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
    total_attempts BIGINT NOT NULL CHECK (total_attempts > 0),
    -- Preserve the original JSON number representation as well as its values.
    receipt_json TEXT NOT NULL CHECK (jsonb_typeof(receipt_json::JSONB) = 'object'
        AND octet_length(receipt_json) <= 1048576),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX training_completion_receipts_user_idx ON training_completion_receipts(user_id);
