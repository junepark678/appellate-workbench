CREATE TABLE store_identity (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    identity TEXT NOT NULL CHECK(length(identity) = 32)
) STRICT;

INSERT INTO store_identity(singleton, identity)
VALUES(1, lower(hex(randomblob(16))));

PRAGMA application_id = 1095784258;
PRAGMA user_version = 3;
