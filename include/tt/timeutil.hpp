#pragma once

#include "tt/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tt
{

    Ts now();

    std::int64_t to_epoch(Ts ts);
    Ts from_epoch(std::int64_t seconds);

    // Example: "2026-08-07 14:32", in the machine's local timezone
    std::string format_local(Ts ts);

    // Example: "45s", "45m", "3h", "3h 12m", "-1h 30m".
    std::string format_duration(Dur d);

    Ts day_start(Ts ts);

    // Accepts "now", "14:30", "2:30pm", "2026-08-07", "2026-08-07 14:30".
    std::optional<Ts> parse_time(std::string_view text, Ts reference_now);

    std::optional<Dur> parse_duration(std::string_view text);

    std::optional<Ts> parse_since(std::string_view text, Ts reference_now);

}
