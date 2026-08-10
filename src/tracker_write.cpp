#include <tt/tracker.hpp>

#include <tt/db.hpp>
#include <tt/json.hpp>
#include <tt/match.hpp>

#include <string>
#include <utility>

namespace tt {
namespace {

void bind_opt(SQLite::Statement& stmt, const char* param,
              const std::optional<std::string>& value) {
    if (value) {
        stmt.bind(param, *value);
    } else {
        stmt.bind(param);
    }
}

// Closes the one running work interval of `task_id`.
// Shared with stopAll: SQLite has no nested transactions.
Interval close_open(SQLite::Database& db, Id task_id, Ts at) {
    SQLite::Statement find(db,
                           "SELECT id, start_ts, note FROM intervals "
                           "WHERE task_id = :task AND kind = 'work' AND end_ts IS NULL");
    find.bind(":task", task_id);
    if (!find.executeStep()) {
        throw Conflict("task " + std::to_string(task_id) + " is not running");
    }

    Interval interval;
    interval.id = find.getColumn(0).getInt64();
    interval.task_id = task_id;
    interval.start = from_epoch(find.getColumn(1).getInt64());
    if (!find.getColumn(2).isNull()) {
        interval.note = find.getColumn(2).getString();
    }
    if (at < interval.start) {
        throw Invalid("stop time is before the interval started");
    }
    interval.end = at;

    SQLite::Statement update(db, "UPDATE intervals SET end_ts = :end WHERE id = :id");
    update.bind(":end", to_epoch(at));
    update.bind(":id", interval.id);
    update.exec();
    return interval;
}

}


Client Tracker::createClient(std::string name) {
    const std::string key = normalize(name);
    if (key.empty()) {
        throw Invalid("a client needs a name");
    }
    SQLite::Transaction txn(db_);
    // Inside the transaction, check and insert see the same rows
    SQLite::Statement existing(db_, "SELECT 1 FROM clients WHERE name_key = :key");
    existing.bind(":key", key);
    if (existing.executeStep()) {
        throw Conflict("a client named '" + name + "' already exists");
    }

    Client client;
    client.id = new_id(db_, "clients");
    client.name = std::move(name);
    client.created_at = clock_();

    SQLite::Statement insert(db_,
                             "INSERT INTO clients (id, name, name_key, created_at) "
                             "VALUES (:id, :name, :key, :created)");
    insert.bind(":id", client.id);
    insert.bind(":name", client.name);
    insert.bind(":key", key);
    insert.bind(":created", to_epoch(client.created_at));
    insert.exec();

    recordMutation("client_created", client);
    txn.commit();
    return client;
}

Task Tracker::createTask(Id client_id, std::string name, std::optional<std::string> description) {
    requireClient(client_id);
    const std::string key = normalize(name);
    if (key.empty()) {
        throw Invalid("a task needs a name");
    }

    SQLite::Transaction txn(db_);
    Task task;
    task.id = new_id(db_, "tasks");
    task.client_id = client_id;
    task.name = std::move(name);
    task.description = std::move(description);
    task.created_at = clock_();

    SQLite::Statement insert(
        db_,
        "INSERT INTO tasks (id, client_id, name, name_key, description, created_at) "
        "VALUES (:id, :client, :name, :key, :description, :created)");
    insert.bind(":id", task.id);
    insert.bind(":client", task.client_id);
    insert.bind(":name", task.name);
    insert.bind(":key", key);
    bind_opt(insert, ":description", task.description);
    insert.bind(":created", to_epoch(task.created_at));
    insert.exec();

    recordMutation("task_created", task);
    txn.commit();
    return task;
}

Interval Tracker::startWork(Id task_id, std::optional<Ts> at) {
    requireTask(task_id);
    SQLite::Statement open(db_,
                           "SELECT 1 FROM intervals WHERE task_id = :task AND end_ts IS NULL");
    open.bind(":task", task_id);
    if (open.executeStep()) {
        throw Conflict("task " + std::to_string(task_id) + " is already running");
    }

    SQLite::Transaction txn(db_);
    Interval interval;
    interval.task_id = task_id;
    interval.kind = IntervalKind::Work;
    interval.start = at.value_or(clock_());

    SQLite::Statement insert(db_,
                             "INSERT INTO intervals (task_id, kind, start_ts, created_at) "
                             "VALUES (:task, 'work', :start, :created)");
    insert.bind(":task", task_id);
    insert.bind(":start", to_epoch(interval.start));
    insert.bind(":created", to_epoch(clock_()));
    insert.exec();
    interval.id = db_.getLastInsertRowid();

    recordMutation("work_started", interval);
    txn.commit();
    return interval;
}

Interval Tracker::stopWork(Id task_id, std::optional<Ts> at) {
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    const Interval interval = close_open(db_, task_id, at.value_or(clock_()));
    recordMutation("work_stopped", interval);
    txn.commit();
    return interval;
}

std::vector<Interval> Tracker::stopAll(std::optional<Ts> at) {
    const Ts when = at.value_or(clock_());

    SQLite::Transaction txn(db_);
    std::vector<Id> task_ids;
    SQLite::Statement running(db_,
                              "SELECT task_id FROM intervals "
                              "WHERE kind = 'work' AND end_ts IS NULL ORDER BY id");
    while (running.executeStep()) {
        task_ids.push_back(running.getColumn(0).getInt64());
    }

    // Any Invalid here aborts the whole sweep => every task stays running
    std::vector<Interval> closed;
    for (const Id task_id : task_ids) {
        closed.push_back(close_open(db_, task_id, when));
    }

    recordMutation("work_stopped_all", {{"count", closed.size()}});
    txn.commit();
    return closed;
}

Interval Tracker::logInterval(Id task_id, Ts start, Ts end, std::optional<std::string> note) {
    requireTask(task_id);
    if (end < start) {
        throw Invalid("interval ends before it starts");
    }

    SQLite::Transaction txn(db_);
    Interval interval;
    interval.task_id = task_id;
    interval.kind = IntervalKind::Work;
    interval.start = start;
    interval.end = end;
    interval.note = std::move(note);

    SQLite::Statement insert(
        db_,
        "INSERT INTO intervals (task_id, kind, start_ts, end_ts, note, created_at) "
        "VALUES (:task, 'work', :start, :end, :note, :created)");
    insert.bind(":task", task_id);
    insert.bind(":start", to_epoch(start));
    insert.bind(":end", to_epoch(end));
    bind_opt(insert, ":note", interval.note);
    insert.bind(":created", to_epoch(clock_()));
    insert.exec();
    interval.id = db_.getLastInsertRowid();

    recordMutation("interval_logged", interval);
    txn.commit();
    return interval;
}


Interval Tracker::addTime(Id task_id, Dur dur, std::optional<std::string> note) {
    if (dur <= Dur{0}) {
        throw Invalid("added time must be positive");
    }
    // One clock read => the span is exactly `dur`
    const Ts end = clock_();
    return logInterval(task_id, end - dur, end, std::move(note));
}

Interval Tracker::adjustTime(Id task_id, Dur delta, std::optional<std::string> note) {
    if (delta == Dur{0}) {
        throw Invalid("an adjustment of zero changes nothing");
    }
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    Interval interval;
    interval.task_id = task_id;
    interval.kind = IntervalKind::Adjustment;
    interval.start = clock_();
    interval.end = interval.start;  // the schema requires it of an adjustment
    interval.delta = delta;
    interval.note = std::move(note);

    SQLite::Statement insert(
        db_,
        "INSERT INTO intervals (task_id, kind, start_ts, end_ts, delta_seconds, note, created_at) "
        "VALUES (:task, 'adjustment', :at, :at, :delta, :note, :at)");
    insert.bind(":task", task_id);
    insert.bind(":at", to_epoch(interval.start));
    insert.bind(":delta", delta.count());
    bind_opt(insert, ":note", interval.note);
    insert.exec();
    interval.id = db_.getLastInsertRowid();

    recordMutation("time_adjusted", interval);
    txn.commit();
    return interval;
}

std::optional<Interval> Tracker::setTotalTime(Id task_id, Dur target,
                                              std::optional<std::string> note) {
    if (target < Dur{0}) {
        throw Invalid("a total cannot be negative");
    }
    requireTask(task_id);

    const std::vector<TaskInfo> rows =
        queryTasks("t.id = :task", "", Range{}, std::nullopt, task_id);
    const Dur delta = target - rows.front().total_all_time;
    if (delta == Dur{0}) {
        return std::nullopt;
    }
    // Only true as of now, a running task keeps accruing past `target`
    return adjustTime(task_id, delta, std::move(note));
}


void Tracker::renameClient(Id client_id, std::string name) {
    requireClient(client_id);
    const std::string key = normalize(name);
    if (key.empty()) {
        throw Invalid("a client needs a name");
    }
    SQLite::Transaction txn(db_);
    // Inside the transaction, as in createClient.
    SQLite::Statement existing(db_, "SELECT 1 FROM clients WHERE name_key = :key AND id != :id");
    existing.bind(":key", key);
    existing.bind(":id", client_id);
    if (existing.executeStep()) {
        throw Conflict("another client is named '" + name + "'");
    }

    SQLite::Statement update(db_,
                             "UPDATE clients SET name = :name, name_key = :key WHERE id = :id");
    update.bind(":name", name);
    update.bind(":key", key);
    update.bind(":id", client_id);
    update.exec();

    recordMutation("client_renamed", {{"client", client_id}, {"name", name}});
    txn.commit();
}

void Tracker::renameTask(Id task_id, std::string name) {
    requireTask(task_id);
    const std::string key = normalize(name);
    if (key.empty()) {
        throw Invalid("a task needs a name");
    }

    SQLite::Transaction txn(db_);
    SQLite::Statement update(db_,
                             "UPDATE tasks SET name = :name, name_key = :key WHERE id = :id");
    update.bind(":name", name);
    update.bind(":key", key);
    update.bind(":id", task_id);
    update.exec();

    recordMutation("task_renamed", {{"task", task_id}, {"name", name}});
    txn.commit();
}

void Tracker::setDescription(Id task_id, std::optional<std::string> description) {
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    SQLite::Statement update(db_, "UPDATE tasks SET description = :description WHERE id = :id");
    bind_opt(update, ":description", description);
    update.bind(":id", task_id);
    update.exec();

    recordMutation("description_set", {{"task", task_id}, {"description", description}});
    txn.commit();
}

void Tracker::finish(Id task_id) {
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    SQLite::Statement update(db_, "UPDATE tasks SET finished = 1 WHERE id = :id");
    update.bind(":id", task_id);
    update.exec();

    recordMutation("task_finished", {{"task", task_id}});
    txn.commit();
}

void Tracker::reopen(Id task_id) {
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    SQLite::Statement update(db_, "UPDATE tasks SET finished = 0 WHERE id = :id");
    update.bind(":id", task_id);
    update.exec();

    recordMutation("task_reopened", {{"task", task_id}});
    txn.commit();
}

Interval Tracker::setIntervalDuration(Id interval_id, Dur seconds) {
    if (seconds.count() <= 0) {
        throw Invalid("a duration must be positive");
    }

    SQLite::Transaction txn(db_);
    SQLite::Statement find(db_,
                           "SELECT id, task_id, kind, start_ts, end_ts, delta_seconds, note "
                           "FROM intervals WHERE id = :id");
    find.bind(":id", interval_id);
    if (!find.executeStep()) {
        throw NotFound("no interval with id " + std::to_string(interval_id));
    }

    Interval interval;
    interval.id = find.getColumn(0).getInt64();
    interval.task_id = find.getColumn(1).getInt64();
    interval.kind = interval_kind_from_string(find.getColumn(2).getString());
    interval.start = from_epoch(find.getColumn(3).getInt64());
    const bool running = find.getColumn(4).isNull();
    if (!find.getColumn(5).isNull()) {
        interval.delta = Dur{find.getColumn(5).getInt64()};
    }
    if (!find.getColumn(6).isNull()) {
        interval.note = find.getColumn(6).getString();
    }

    // An adjustment is zero-width by schema, its size lives in delta_seconds
    if (interval.kind == IntervalKind::Adjustment) {
        throw Invalid("an adjustment has no span");
    }
    if (running) {
        throw Conflict("interval " + std::to_string(interval_id) + " is still running");
    }
    interval.end = interval.start + seconds;

    SQLite::Statement update(db_,
                             "UPDATE intervals SET end_ts = start_ts + :seconds WHERE id = :id");
    update.bind(":seconds", seconds.count());
    update.bind(":id", interval_id);
    update.exec();

    recordMutation("interval_duration_set", interval);
    txn.commit();
    return interval;
}

void Tracker::deleteInterval(Id interval_id) {
    SQLite::Transaction txn(db_);
    SQLite::Statement remove(db_, "DELETE FROM intervals WHERE id = :id");
    remove.bind(":id", interval_id);
    if (remove.exec() == 0) {
        throw NotFound("no interval with id " + std::to_string(interval_id));
    }

    recordMutation("interval_deleted", {{"interval", interval_id}});
    txn.commit();
}

void Tracker::deleteTask(Id task_id) {
    requireTask(task_id);

    SQLite::Transaction txn(db_);
    SQLite::Statement remove(db_, "DELETE FROM tasks WHERE id = :id");
    remove.bind(":id", task_id);
    remove.exec();

    recordMutation("task_deleted", {{"task", task_id}});
    txn.commit();
}

void Tracker::deleteClient(Id client_id) {
    requireClient(client_id);

    SQLite::Transaction txn(db_);
    SQLite::Statement remove(db_, "DELETE FROM clients WHERE id = :id");
    remove.bind(":id", client_id);
    remove.exec();

    recordMutation("client_deleted", {{"client", client_id}});
    txn.commit();
}

}
