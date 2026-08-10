#pragma once

#include "tt/timeutil.hpp"
#include "tt/types.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tt
{

    class Tracker
    {
    public:
        using Clock = std::function<Ts()>;

        explicit Tracker(const std::string &db_path, Clock clock = Clock(&tt::now));

        Tracker(const Tracker &) = delete;
        Tracker &operator=(const Tracker &) = delete;

        // Constructing inserts the row, destroying stamps ended_at
        class Session
        {
        public:
            Session(const Session &) = delete;
            Session &operator=(const Session &) = delete;
            Session(Session &&other) noexcept;
            Session &operator=(Session &&other) noexcept;
            ~Session();

            [[nodiscard]] Id id() const noexcept { return id_; }

        private:
            friend class Tracker;
            Session(Tracker &owner, Id id);

            Tracker *owner_; // null once moved from
            Id id_;
        };

        [[nodiscard]] Ts now() const { return clock_(); }

        [[nodiscard]] std::vector<TaskInfo> find(const Query &query = {}) const;
        [[nodiscard]] std::optional<TaskInfo> getTask(Id task_id, Range range = {}) const;
        [[nodiscard]] std::optional<ClientDetail> getClient(Id client_id, Range range = {}) const;
        [[nodiscard]] std::vector<Client> listClients() const;
        // Ranks clients and tasks against a spoken phrase, exact first, then by score
        [[nodiscard]] Resolution resolve(std::string_view phrase,
                                         std::optional<EntityKind> kind = std::nullopt,
                                         std::optional<Id> client_scope = std::nullopt) const;

        Client createClient(std::string name);
        Task createTask(Id client_id, std::string name,
                        std::optional<std::string> description = std::nullopt);
        // Opens a work interval. Throws Conflict if one is already running
        Interval startWork(Id task_id, std::optional<Ts> at = std::nullopt);
        // Closes the running work interval. Throws Conflict if none is
        Interval stopWork(Id task_id, std::optional<Ts> at = std::nullopt);
        // Closes every running interval; empty if nothing was running
        std::vector<Interval> stopAll(std::optional<Ts> at = std::nullopt);
        // Records work that already happened
        Interval logInterval(Id task_id, Ts start, Ts end,
                             std::optional<std::string> note = std::nullopt);

        // `dur` of work ending now, as a work interval [now - dur, now]
        Interval addTime(Id task_id, Dur dur, std::optional<std::string> note = std::nullopt);
        // Adds an adjustment
        Interval adjustTime(Id task_id, Dur delta, std::optional<std::string> note = std::nullopt);
        // Adjusts the all-time total to `target`, nullopt if it already is.
        std::optional<Interval> setTotalTime(Id task_id, Dur target,
                                             std::optional<std::string> note = std::nullopt);

        void renameClient(Id client_id, std::string name);
        void renameTask(Id task_id, std::string name);
        void setDescription(Id task_id, std::optional<std::string> description);
        void finish(Id task_id);
        void reopen(Id task_id);
        Interval setIntervalDuration(Id interval_id, Dur seconds);
        void deleteInterval(Id interval_id);
        void deleteTask(Id task_id);
        void deleteClient(Id client_id);

        [[nodiscard]] Session session(std::optional<std::string> label = std::nullopt);
        Id record(std::string_view type, const nlohmann::json &payload);
        [[nodiscard]] std::vector<Event> recent(int limit = 50) const;
        [[nodiscard]] std::optional<tt::Session> lastSession() const;

    private:
        // Journals payload only when a session is open.
        void recordMutation(std::string_view type, const nlohmann::json &payload);
        std::optional<Client> findClient(Id id) const;
        Client requireClient(Id id) const;
        Task requireTask(Id id) const;
        void endSession(Id id);

        // `where_extra` and `having_extra` are fixed clause text, naming only :client and :task.
        std::vector<TaskInfo> queryTasks(const std::string &where_extra,
                                         const std::string &having_extra,
                                         const Range &range,
                                         std::optional<Id> client,
                                         std::optional<Id> task) const;
        std::vector<Interval> loadIntervals(Id task_id) const;

        SQLite::Database db_;
        Clock clock_;
        std::optional<Id> current_session_;
    };

}
