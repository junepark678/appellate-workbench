CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at_utc TEXT NOT NULL
) STRICT;

CREATE TABLE installed_pack_revisions (
    pack_id TEXT NOT NULL,
    version TEXT NOT NULL,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    installed_at_utc TEXT NOT NULL,
    PRIMARY KEY (pack_id, version),
    UNIQUE (pack_id, version, digest)
) STRICT;

CREATE TABLE sessions (
    session_id TEXT PRIMARY KEY,
    engine_revision TEXT NOT NULL,
    sequence INTEGER NOT NULL DEFAULT 0 CHECK(sequence >= 0),
    created_at_utc TEXT NOT NULL
) STRICT;

CREATE TABLE session_pins (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    pack_id TEXT NOT NULL,
    version TEXT NOT NULL,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    PRIMARY KEY (session_id, pack_id)
) STRICT;

CREATE TABLE command_log (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    command_id TEXT NOT NULL,
    expected_sequence INTEGER NOT NULL CHECK(expected_sequence >= 0),
    payload_json BLOB NOT NULL,
    recorded_at_utc TEXT NOT NULL,
    PRIMARY KEY (session_id, command_id)
) STRICT;

CREATE TABLE event_log (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK(sequence > 0),
    event_type TEXT NOT NULL,
    payload_json BLOB NOT NULL,
    authority_id TEXT NOT NULL,
    PRIMARY KEY (session_id, sequence)
) STRICT;

CREATE TABLE docket_projection (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    entry_id TEXT NOT NULL,
    event_sequence INTEGER NOT NULL CHECK(event_sequence > 0),
    title TEXT NOT NULL,
    status TEXT NOT NULL,
    PRIMARY KEY (session_id, entry_id),
    FOREIGN KEY (session_id, event_sequence)
        REFERENCES event_log(session_id, sequence) ON DELETE CASCADE
) STRICT;

CREATE TABLE asset_references (
    session_id TEXT NOT NULL REFERENCES sessions(session_id) ON DELETE CASCADE,
    digest TEXT NOT NULL CHECK(length(digest) = 64),
    purpose TEXT NOT NULL,
    PRIMARY KEY (session_id, digest, purpose)
) STRICT;
