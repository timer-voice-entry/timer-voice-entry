#pragma once

#include "tt/errors.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tt {

// The random six-digit key drawn by new_id().
using Id = std::int64_t;

// A point in time: UTC, second precision.
using Ts = std::chrono::sys_seconds;

// A length of time. Signed; adjustments may be negative.
using Dur = std::chrono::seconds;

enum class IntervalKind { Work, Adjustment };

enum class EntityKind { Client, Task };

// Which text produced a search hit.
enum class MatchField { Name, Description };

// None first => a default Resolution reads as "found nothing", not "certain".
enum class ResolveVerdict { None, Ambiguous, Unique };

// "unknown" is not a valid schema value => a corrupt enum fails loudly

inline std::string_view to_string(IntervalKind kind) {
    switch (kind) {
        case IntervalKind::Work:       return "work";
        case IntervalKind::Adjustment: return "adjustment";
    }
    return "unknown";
}

inline std::string_view to_string(EntityKind kind) {
    switch (kind) {
        case EntityKind::Client: return "client";
        case EntityKind::Task:   return "task";
    }
    return "unknown";
}

inline std::string_view to_string(MatchField field) {
    switch (field) {
        case MatchField::Name:        return "name";
        case MatchField::Description: return "description";
    }
    return "unknown";
}

inline std::string_view to_string(ResolveVerdict verdict) {
    switch (verdict) {
        case ResolveVerdict::None:      return "none";
        case ResolveVerdict::Ambiguous: return "ambiguous";
        case ResolveVerdict::Unique:    return "unique";
    }
    return "unknown";
}

// Only the two spellings the schema's CHECK constraint allows.
inline IntervalKind interval_kind_from_string(std::string_view text) {
    if (text == "work") {
        return IntervalKind::Work;
    }
    if (text == "adjustment") {
        return IntervalKind::Adjustment;
    }
    throw Invalid("unknown interval kind '" + std::string(text) + "'");
}

// A half-open time window. Either end absent => unbounded.
struct Range {
    std::optional<Ts> from{};
    std::optional<Ts> to{};
};

struct Client {
    Id id{};
    std::string name{};
    Ts created_at{};
};

struct Task {
    Id id{};
    Id client_id{};
    std::string name{};
    std::optional<std::string> description{};
    bool finished{};
    Ts created_at{};
};

struct Interval {
    Id id{};
    Id task_id{};
    IntervalKind kind = IntervalKind::Work;
    Ts start{};
    // Absent => running right now.
    std::optional<Ts> end{};
    // Signed; meaningful only when kind == Adjustment.
    Dur delta{};
    std::optional<std::string> note{};

    [[nodiscard]] bool running() const noexcept {
        return kind == IntervalKind::Work && !end.has_value();
    }
};

// A task plus its rolled-up time.
struct TaskInfo {
    Task task{};
    std::string client_name{};
    // Clipped to the query window.
    Dur total_in_range{};
    // Lifetime, ignoring the window.
    Dur total_all_time{};
    bool running{};
    std::optional<Ts> last_worked{};
    // Empty in list results; set only when a single task is fetched.
    std::vector<Interval> intervals{};
};

struct ClientDetail {
    Client client{};
    std::vector<TaskInfo> tasks{};
    Dur total_in_range{};
    Dur total_all_time{};
};

// Filters for a task listing. Absent field => no filter.
struct Query {
    std::optional<Id> client{};
    Range range{};
    bool include_finished = true;
    bool only_running = false;
};

// One ranked search hit. Example: "the roof job you worked on Tuesday" instead of an id.
struct Candidate {
    EntityKind kind{};
    Id id{};
    std::string name{};
    // Both set only when kind == Task.
    Id client_id{};
    std::string client_name{};
    MatchField matched_field{};
    // 1.0 means exact after normalization.
    double score{};
    // Whole-name equality after normalization. score can't tell: a partial match hits 1.0 too.
    bool exact{};
    std::optional<Ts> last_worked{};
    Dur total_all_time{};
    bool finished{};
};

struct Resolution {
    ResolveVerdict verdict{};
    // Ranked, best first.
    std::vector<Candidate> candidates{};
};

struct Session {
    Id id{};
    std::optional<std::string> label{};
    Ts started_at{};
    std::optional<Ts> ended_at{};
};

struct Event {
    Id id{};
    Id session_id{};
    Ts ts{};
    std::string type{};
    nlohmann::json payload{};
};

}
