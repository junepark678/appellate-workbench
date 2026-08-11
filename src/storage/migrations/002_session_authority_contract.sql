ALTER TABLE sessions
    ADD COLUMN authority_contract TEXT NOT NULL DEFAULT 'legacy-v1'
        CHECK(authority_contract IN ('legacy-v1', 'canonical-v2'));
