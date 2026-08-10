#pragma once

#include "tt/timeutil.hpp"
#include "tt/types.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace nlohmann
{

    // An empty optional goes out as null
    template <typename T>
    struct adl_serializer<std::optional<T>>
    {
        static void to_json(json &j, const std::optional<T> &opt)
        {
            if (opt)
            {
                j = *opt;
            }
            else
            {
                j = nullptr;
            }
        }
    };

    // Epoch seconds
    template <>
    struct adl_serializer<tt::Ts>
    {
        static void to_json(json &j, tt::Ts ts) { j = tt::to_epoch(ts); }
    };

    template <>
    struct adl_serializer<tt::Dur>
    {
        static void to_json(json &j, tt::Dur d) { j = d.count(); }
    };

}

namespace tt
{

    // Same words to_string() uses
    inline void to_json(nlohmann::json &j, IntervalKind v) { j = to_string(v); }
    inline void to_json(nlohmann::json &j, EntityKind v) { j = to_string(v); }
    inline void to_json(nlohmann::json &j, MatchField v) { j = to_string(v); }
    inline void to_json(nlohmann::json &j, ResolveVerdict v) { j = to_string(v); }

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Client, id, name, created_at)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Task, id, client_id, name, description, finished,
                                                      created_at)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Interval, id, task_id, kind, start, end, delta,
                                                      note)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(TaskInfo, task, client_name, total_in_range,
                                                      total_all_time, running, last_worked, intervals)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(ClientDetail, client, tasks, total_in_range,
                                                      total_all_time)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Candidate, kind, id, name, client_id, client_name,
                                                      matched_field, score, exact, last_worked,
                                                      total_all_time, finished)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Resolution, verdict, candidates)
    // Event::payload is already json
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Event, id, session_id, ts, type, payload)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_ONLY_SERIALIZE(Session, id, label, started_at, ended_at)

}
