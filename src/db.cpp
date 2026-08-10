#include <tt/db.hpp>

#include <random>
#include <stdexcept>
#include <string>

namespace tt {
namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS clients (
    id         INTEGER PRIMARY KEY CHECK (id BETWEEN 100000 AND 999999),
    name       TEXT    NOT NULL,
    name_key   TEXT    NOT NULL UNIQUE,
    created_at INTEGER NOT NULL
);

CREATE TRIGGER IF NOT EXISTS trg_clients_id_immutable
BEFORE UPDATE OF id ON clients
BEGIN
    SELECT RAISE(ABORT, 'client id is immutable');
END;

CREATE TABLE IF NOT EXISTS tasks (
    id          INTEGER PRIMARY KEY CHECK (id BETWEEN 100000 AND 999999),
    client_id   INTEGER NOT NULL REFERENCES clients(id) ON DELETE CASCADE,
    name        TEXT    NOT NULL,
    name_key    TEXT    NOT NULL,
    description TEXT,
    finished    INTEGER NOT NULL DEFAULT 0 CHECK (finished IN (0, 1)),
    created_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_tasks_client   ON tasks(client_id);
CREATE INDEX IF NOT EXISTS idx_tasks_name_key ON tasks(name_key);

-- A task's id and client never change. Its name does; speech mishears names.
CREATE TRIGGER IF NOT EXISTS trg_tasks_identity_immutable
BEFORE UPDATE OF id, client_id ON tasks
BEGIN
    SELECT RAISE(ABORT, 'task identity (id, client) is immutable');
END;

CREATE TABLE IF NOT EXISTS intervals (
    id            INTEGER PRIMARY KEY,
    task_id       INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
    kind          TEXT    NOT NULL CHECK (kind IN ('work', 'adjustment')),
    start_ts      INTEGER NOT NULL,
    end_ts        INTEGER,
    delta_seconds INTEGER,
    note          TEXT,
    created_at    INTEGER NOT NULL,

    -- One kind of duration per row:
    --   work       -> COALESCE(end_ts, now) - start_ts
    --   adjustment -> delta_seconds, a signed correction
    CHECK (
        (kind = 'work'       AND delta_seconds IS NULL
                             AND (end_ts IS NULL OR end_ts >= start_ts))
     OR (kind = 'adjustment' AND delta_seconds IS NOT NULL
                             AND end_ts = start_ts)
    )
);

CREATE INDEX IF NOT EXISTS idx_intervals_task ON intervals(task_id, start_ts);

-- One open interval per task. Adjustments are zero-width, never collide here.
CREATE UNIQUE INDEX IF NOT EXISTS idx_intervals_one_open
    ON intervals(task_id) WHERE end_ts IS NULL;

CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY,
    label      TEXT,
    started_at INTEGER NOT NULL,
    ended_at   INTEGER
);

CREATE TABLE IF NOT EXISTS events (
    id         INTEGER PRIMARY KEY,
    session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,
    ts         INTEGER NOT NULL,
    type       TEXT    NOT NULL,
    payload    TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_id, id);
)SQL";

}

SQLite::Database open_database(const std::string& path) {
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

    db.exec("PRAGMA foreign_keys = ON");
    db.exec("PRAGMA journal_mode = WAL");

    const int version = db.execAndGet("PRAGMA user_version").getInt();
    if (version > kSchemaVersion) {
        throw std::runtime_error(
            "database schema version " + std::to_string(version) +
            " is newer than supported version " + std::to_string(kSchemaVersion));
    }
    if (version == kSchemaVersion) {
        return db;
    }

    SQLite::Transaction txn(db);
    db.exec(kSchema);
    // PRAGMA arguments cannot be bound
    db.exec("PRAGMA user_version = " + std::to_string(kSchemaVersion));
    txn.commit();

    return db;
}

std::int64_t new_id(SQLite::Database& db, const std::string& table) {
    // The table name is interpolated into the SQL below, not bound
    if (table != "clients" && table != "tasks") {
        throw std::invalid_argument("new_id: unsupported table '" + table + "'");
    }

    static thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<std::int64_t> dist(kMinId, kMaxId);

    // Check-then-insert: safe only inside the caller's transaction; the primary key is the real guard
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::int64_t candidate = dist(rng);
        SQLite::Statement query(db, "SELECT 1 FROM " + table + " WHERE id = ?");
        query.bind(1, candidate);
        if (!query.executeStep()) {
            return candidate;
        }
    }

    throw std::runtime_error("no free id found for table '" + table + "'");
}

}
