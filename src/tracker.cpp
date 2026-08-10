#include <tt/tracker.hpp>

#include <tt/db.hpp>
#include <tt/match.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace tt {
namespace {

// Epoch ints stay in this file, everything above uses Ts. These two mean "unbounded" to SQL.
constexpr std::int64_t kMinEpoch = -62135596800;  // year 1
constexpr std::int64_t kMaxEpoch = 253402300799;  // year 9999

// Every total is a SUM of this. Work clips to the window, running at :now, adjustments by instant.
std::string sum_expr(const char* from_param, const char* to_param) {
    const std::string from(from_param);
    const std::string to(to_param);
    return "CASE WHEN i.kind = 'adjustment' "
           "THEN CASE WHEN i.start_ts >= " + from + " AND i.start_ts < " + to +
           " THEN i.delta_seconds ELSE 0 END "
           "ELSE MAX(0, MIN(COALESCE(i.end_ts, :now), " + to +
           ") - MAX(i.start_ts, " + from + ")) END";
}

// LEFT JOIN => a task with no intervals still shows up, at zero.
std::string task_query(const std::string& where_extra, const std::string& having_extra) {
    std::string sql =
        "SELECT t.id, t.client_id, t.name, t.description, t.finished, t.created_at, "
        "c.name AS client_name, "
        "COALESCE(SUM(" + sum_expr(":from", ":to") + "), 0) AS total_in_range, "
        "COALESCE(SUM(" + sum_expr(":allfrom", ":allto") + "), 0) AS total_all_time, "
        "COALESCE(MAX(CASE WHEN i.kind = 'work' AND i.end_ts IS NULL "
        "THEN 1 ELSE 0 END), 0) AS running, "
        "MAX(CASE WHEN i.kind = 'work' THEN COALESCE(i.end_ts, i.start_ts) END) AS last_worked "
        "FROM tasks t "
        "JOIN clients c ON c.id = t.client_id "
        "LEFT JOIN intervals i ON i.task_id = t.id";
    if (!where_extra.empty()) {
        sql += " WHERE " + where_extra;
    }
    sql += " GROUP BY t.id";
    if (!having_extra.empty()) {
        sql += " HAVING " + having_extra;
    }
    return sql + " ORDER BY c.name, t.name";
}

Client read_client(SQLite::Statement& stmt) {
    Client client;
    client.id = stmt.getColumn(0).getInt64();
    client.name = stmt.getColumn(1).getString();
    client.created_at = from_epoch(stmt.getColumn(2).getInt64());
    return client;
}

Event read_event(SQLite::Statement& stmt) {
    Event event;
    event.id = stmt.getColumn(0).getInt64();
    // Journalled outside any session => no session to point at
    event.session_id = stmt.getColumn(1).isNull() ? 0 : stmt.getColumn(1).getInt64();
    event.ts = from_epoch(stmt.getColumn(2).getInt64());
    event.type = stmt.getColumn(3).getString();
    event.payload = nlohmann::json::parse(stmt.getColumn(4).getString());
    return event;
}

}

Tracker::Tracker(const std::string& db_path, Clock clock)
    : db_(open_database(db_path)), clock_(std::move(clock)) {}


Tracker::Session::Session(Tracker& owner, Id id) : owner_(&owner), id_(id) {}

Tracker::Session::Session(Session&& other) noexcept : owner_(other.owner_), id_(other.id_) {
    other.owner_ = nullptr;
}

Tracker::Session& Tracker::Session::operator=(Session&& other) noexcept {
    // Swap, not end-then-adopt: endSession() runs SQL and this is noexcept
    std::swap(owner_, other.owner_);
    std::swap(id_, other.id_);
    return *this;
}

Tracker::Session::~Session() {
    if (owner_ == nullptr) {
        return;
    }
    try {
        owner_->endSession(id_);
    } catch (...) {
        // A destructor may not throw. Better to lose the ended_at stamp than crash.
    }
}


std::vector<TaskInfo> Tracker::queryTasks(const std::string& where_extra,
                                          const std::string& having_extra,
                                          const Range& range,
                                          std::optional<Id> client,
                                          std::optional<Id> task) const {
    SQLite::Statement stmt(db_, task_query(where_extra, having_extra));
    stmt.bind(":now", to_epoch(clock_()));
    stmt.bind(":from", range.from ? to_epoch(*range.from) : kMinEpoch);
    stmt.bind(":to", range.to ? to_epoch(*range.to) : kMaxEpoch);
    stmt.bind(":allfrom", kMinEpoch);
    stmt.bind(":allto", kMaxEpoch);
    // Only when the clause text names them, SQLiteCpp throws on a parameter it does not contain
    if (client) {
        stmt.bind(":client", *client);
    }
    if (task) {
        stmt.bind(":task", *task);
    }

    std::vector<TaskInfo> out;
    while (stmt.executeStep()) {
        TaskInfo info;
        info.task.id = stmt.getColumn(0).getInt64();
        info.task.client_id = stmt.getColumn(1).getInt64();
        info.task.name = stmt.getColumn(2).getString();
        if (!stmt.getColumn(3).isNull()) {
            info.task.description = stmt.getColumn(3).getString();
        }
        info.task.finished = stmt.getColumn(4).getInt() != 0;
        info.task.created_at = from_epoch(stmt.getColumn(5).getInt64());
        info.client_name = stmt.getColumn(6).getString();
        info.total_in_range = Dur{stmt.getColumn(7).getInt64()};
        info.total_all_time = Dur{stmt.getColumn(8).getInt64()};
        info.running = stmt.getColumn(9).getInt() != 0;
        if (!stmt.getColumn(10).isNull()) {
            info.last_worked = from_epoch(stmt.getColumn(10).getInt64());
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<Interval> Tracker::loadIntervals(Id task_id) const {
    SQLite::Statement stmt(db_,
                           "SELECT id, task_id, kind, start_ts, end_ts, delta_seconds, note "
                           "FROM intervals WHERE task_id = :task ORDER BY start_ts, id");
    stmt.bind(":task", task_id);

    std::vector<Interval> out;
    while (stmt.executeStep()) {
        Interval interval;
        interval.id = stmt.getColumn(0).getInt64();
        interval.task_id = stmt.getColumn(1).getInt64();
        interval.kind = interval_kind_from_string(stmt.getColumn(2).getString());
        interval.start = from_epoch(stmt.getColumn(3).getInt64());
        if (!stmt.getColumn(4).isNull()) {
            interval.end = from_epoch(stmt.getColumn(4).getInt64());
        }
        if (!stmt.getColumn(5).isNull()) {
            interval.delta = Dur{stmt.getColumn(5).getInt64()};
        }
        if (!stmt.getColumn(6).isNull()) {
            interval.note = stmt.getColumn(6).getString();
        }
        out.push_back(std::move(interval));
    }
    return out;
}

std::vector<TaskInfo> Tracker::find(const Query& query) const {
    std::string where;
    if (query.client) {
        where = "t.client_id = :client";
    }
    if (!query.include_finished) {
        if (!where.empty()) {
            where += " AND ";
        }
        where += "t.finished = 0";
    }

    std::string having;
    if (query.only_running) {
        having = "running = 1";
    }
    if (query.range.from || query.range.to) {
        // A window drops tasks with no activity inside. No window => never-worked tasks still list.
        if (!having.empty()) {
            having += " AND ";
        }
        having +=
            "SUM(CASE WHEN i.kind = 'adjustment' "
            "THEN (i.start_ts >= :from AND i.start_ts < :to) "
            "ELSE (COALESCE(i.end_ts, :now) > :from AND i.start_ts < :to) END) > 0";
    }

    return queryTasks(where, having, query.range, query.client, std::nullopt);
}

std::optional<TaskInfo> Tracker::getTask(Id task_id, Range range) const {
    std::vector<TaskInfo> rows = queryTasks("t.id = :task", "", range, std::nullopt, task_id);
    if (rows.empty()) {
        return std::nullopt;
    }
    TaskInfo info = std::move(rows.front());
    info.intervals = loadIntervals(task_id);
    return info;
}

std::optional<ClientDetail> Tracker::getClient(Id client_id, Range range) const {
    std::optional<Client> client = findClient(client_id);
    if (!client) {
        return std::nullopt;
    }

    ClientDetail detail;
    detail.client = *std::move(client);
    detail.tasks = queryTasks("t.client_id = :client", "", range, client_id, std::nullopt);
    for (const TaskInfo& task : detail.tasks) {
        detail.total_in_range += task.total_in_range;
        detail.total_all_time += task.total_all_time;
    }
    return detail;
}

std::vector<Client> Tracker::listClients() const {
    SQLite::Statement stmt(db_, "SELECT id, name, created_at FROM clients ORDER BY name");
    std::vector<Client> out;
    while (stmt.executeStep()) {
        out.push_back(read_client(stmt));
    }
    return out;
}

Resolution Tracker::resolve(std::string_view phrase, std::optional<EntityKind> kind,
                            std::optional<Id> client_scope) const {
    if (normalize(phrase).empty()) { return {ResolveVerdict::None, {}}; }
    // One listing does both jobs: the task candidates, and the client rollups
    const std::vector<TaskInfo> tasks = queryTasks(
        client_scope ? "t.client_id = :client" : "", "", Range{}, client_scope, std::nullopt);
    std::vector<Candidate> candidates;

    // Client already settled by the scope => no client candidates
    if (kind != EntityKind::Task && !client_scope) {
        for (const Client& client : listClients()) {
            const double score = phrase_score(phrase, client.name);
            if (score < kMinPhraseScore) { continue; }

            Candidate out{.kind = EntityKind::Client, .id = client.id, .name = client.name,
                          .matched_field = MatchField::Name, .score = score,
                          .exact = exact_match(phrase, client.name)};
            // A client's time is its tasks' time. `finished` has no meaning here.
            for (const TaskInfo& task : tasks) {
                if (task.task.client_id != client.id) { continue; }
                out.total_all_time += task.total_all_time;
                if (task.last_worked &&
                    (!out.last_worked || *task.last_worked > *out.last_worked)) {
                    out.last_worked = task.last_worked;
                }
            }
            candidates.push_back(std::move(out));
        }
    }
    if (kind != EntityKind::Client) {
        for (const TaskInfo& task : tasks) {
            // Scored against "client task" together. Example: "the Northgate roof"
            const double name_score = phrase_score(phrase, task.client_name + " " + task.task.name);
            const double desc_score = task.task.description
                ? phrase_score(phrase, *task.task.description) * kDescriptionWeight : 0.0;
            const bool by_name = name_score >= desc_score;  // a tie goes to the name
            const double score = by_name ? name_score : desc_score;
            if (score < kMinPhraseScore) { continue; }

            candidates.push_back({.kind = EntityKind::Task, .id = task.task.id,
                                  .name = task.task.name, .client_id = task.task.client_id,
                                  .client_name = task.client_name,
                                  .matched_field = by_name ? MatchField::Name
                                                           : MatchField::Description,
                                  .score = score,
                                  .exact = exact_match(phrase, task.task.name),
                                  .last_worked = task.last_worked,
                                  .total_all_time = task.total_all_time,
                                  .finished = task.task.finished});
        }
    }
    // Exact first => candidates.front() is the match on Unique. Score can't, a partial hits 1.0.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.exact != b.exact) { return a.exact; }
        if (a.score != b.score) { return a.score > b.score; }
        return a.name < b.name;  // ties broken by name, stable order
    });
    // Exactness came from the candidate's own name, not the concatenation that scored it
    const auto exact = std::ranges::count(candidates, true, &Candidate::exact);
    // Survivors but no single exact name => Ambiguous, confirm it. exact > 1 lands here too.
    const ResolveVerdict verdict = exact == 1            ? ResolveVerdict::Unique
                                   : candidates.empty() ? ResolveVerdict::None
                                                        : ResolveVerdict::Ambiguous;
    return {verdict, std::move(candidates)};
}


Tracker::Session Tracker::session(std::optional<std::string> label) {
    // No nesting, a mutation belongs to exactly one session
    if (current_session_) {
        throw Conflict("a session is already open");
    }

    SQLite::Statement stmt(db_,
                           "INSERT INTO sessions (label, started_at) VALUES (:label, :started)");
    if (label) {
        stmt.bind(":label", *label);
    } else {
        stmt.bind(":label");
    }
    stmt.bind(":started", to_epoch(clock_()));
    stmt.exec();

    const Id id = db_.getLastInsertRowid();
    current_session_ = id;
    return Session(*this, id);
}

void Tracker::endSession(Id id) {
    SQLite::Statement stmt(db_, "UPDATE sessions SET ended_at = :ended WHERE id = :id");
    stmt.bind(":ended", to_epoch(clock_()));
    stmt.bind(":id", id);
    stmt.exec();
    if (current_session_ == id) {
        current_session_.reset();
    }
}

Id Tracker::record(std::string_view type, const nlohmann::json& payload) {
    SQLite::Statement stmt(db_,
                           "INSERT INTO events (session_id, ts, type, payload) "
                           "VALUES (:session, :ts, :type, :payload)");
    if (current_session_) {
        stmt.bind(":session", *current_session_);
    } else {
        stmt.bind(":session");
    }
    stmt.bind(":ts", to_epoch(clock_()));
    stmt.bind(":type", std::string(type));
    stmt.bind(":payload", payload.dump());
    stmt.exec();
    return db_.getLastInsertRowid();
}

void Tracker::recordMutation(std::string_view type, const nlohmann::json& payload) {
    // Nothing to attach the entry to outside a session
    if (current_session_) {
        record(type, payload);
    }
}

std::vector<Event> Tracker::recent(int limit) const {
    SQLite::Statement stmt(db_,
                           "SELECT id, session_id, ts, type, payload FROM events "
                           "ORDER BY id DESC LIMIT :limit");
    stmt.bind(":limit", limit);

    std::vector<Event> out;
    while (stmt.executeStep()) {
        out.push_back(read_event(stmt));
    }
    return out;
}

std::optional<tt::Session> Tracker::lastSession() const {
    SQLite::Statement stmt(
        db_, "SELECT id, label, started_at, ended_at FROM sessions ORDER BY id DESC LIMIT 1");
    if (!stmt.executeStep()) {
        return std::nullopt;
    }

    tt::Session session;
    session.id = stmt.getColumn(0).getInt64();
    if (!stmt.getColumn(1).isNull()) {
        session.label = stmt.getColumn(1).getString();
    }
    session.started_at = from_epoch(stmt.getColumn(2).getInt64());
    if (!stmt.getColumn(3).isNull()) {
        session.ended_at = from_epoch(stmt.getColumn(3).getInt64());
    }
    return session;
}


std::optional<Client> Tracker::findClient(Id id) const {
    SQLite::Statement stmt(db_, "SELECT id, name, created_at FROM clients WHERE id = :id");
    stmt.bind(":id", id);
    if (!stmt.executeStep()) {
        return std::nullopt;
    }
    return read_client(stmt);
}

Client Tracker::requireClient(Id id) const {
    std::optional<Client> client = findClient(id);
    if (!client) {
        throw NotFound("no client with id " + std::to_string(id));
    }
    return *std::move(client);
}

Task Tracker::requireTask(Id id) const {
    SQLite::Statement stmt(db_,
                           "SELECT id, client_id, name, description, finished, created_at "
                           "FROM tasks WHERE id = :id");
    stmt.bind(":id", id);
    if (!stmt.executeStep()) {
        throw NotFound("no task with id " + std::to_string(id));
    }

    Task task;
    task.id = stmt.getColumn(0).getInt64();
    task.client_id = stmt.getColumn(1).getInt64();
    task.name = stmt.getColumn(2).getString();
    if (!stmt.getColumn(3).isNull()) {
        task.description = stmt.getColumn(3).getString();
    }
    task.finished = stmt.getColumn(4).getInt() != 0;
    task.created_at = from_epoch(stmt.getColumn(5).getInt64());
    return task;
}

}
