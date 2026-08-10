#include <tt/timeutil.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace tt
{
    namespace
    {

        constexpr const char *kSpace = " \t\r\n";

        std::string_view trim(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(kSpace);
            if (first == std::string_view::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        }

        std::string lower(std::string_view text)
        {
            std::string out(text);
            for (char &c : out)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return out;
        }

        bool is_digit(char c) { return c >= '0' && c <= '9'; }

        // Advances `i` past a run of digits, returns how many.
        std::size_t scan_digits(const std::string &s, std::size_t &i)
        {
            const std::size_t start = i;
            while (i < s.size() && is_digit(s[i]))
            {
                ++i;
            }
            return i - start;
        }

        std::tm local_tm(Ts ts)
        {
            const std::time_t t = static_cast<std::time_t>(to_epoch(ts));
            std::tm out{};
            ::localtime_r(&t, &out);
            return out;
        }

        // tm_isdst = -1 lets the system resolve DST
        std::optional<Ts> to_ts(std::tm &parts)
        {
            parts.tm_isdst = -1;
            const std::time_t t = std::mktime(&parts);
            if (t == static_cast<std::time_t>(-1))
            {
                return std::nullopt;
            }
            return from_epoch(static_cast<std::int64_t>(t));
        }

        // Fills y/mo/d from "YYYY-MM-DD", whole string consumed.
        bool parse_date(const std::string &text, int &y, int &mo, int &d)
        {
            int consumed = -1;
            return std::sscanf(text.c_str(), "%d-%d-%d%n", &y, &mo, &d, &consumed) == 3 &&
                   consumed == static_cast<int>(text.size());
        }

        // Local timestamp from calendar fields, nullopt if a field is out of range.
        std::optional<Ts> from_parts(int y, int mo, int d, int h, int mi)
        {
            if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
            {
                return std::nullopt;
            }
            std::tm parts{};
            parts.tm_year = y - 1900;
            parts.tm_mon = mo - 1;
            parts.tm_mday = d;
            parts.tm_hour = h;
            parts.tm_min = mi;
            const std::optional<Ts> ts = to_ts(parts);
            // Recheck after mktime: it normalises 2026-02-31 to March 3
            if (parts.tm_mon != mo - 1 || parts.tm_mday != d)
            {
                return std::nullopt;
            }
            return ts;
        }

    }

    Ts now()
    {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    }

    std::int64_t to_epoch(Ts ts) { return ts.time_since_epoch().count(); }

    Ts from_epoch(std::int64_t seconds) { return Ts{std::chrono::seconds{seconds}}; }

    std::string format_local(Ts ts)
    {
        // strftime, not std::format/current_zone: libc++ on Apple clang
        std::tm parts = local_tm(ts);
        char buffer[32];
        const std::size_t n = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts);
        return std::string(buffer, n);
    }

    std::string format_duration(Dur d)
    {
        if (d.count() == 0)
        {
            return "0m";
        }

        // Magnitude only. Signed division gives "-1h -30m".
        const std::int64_t total = d.count() < 0 ? -d.count() : d.count();
        const std::string sign = d.count() < 0 ? "-" : "";

        if (total < 60)
        {
            return sign + std::to_string(total) + "s";
        }

        const std::int64_t hours = total / 3600;
        const std::int64_t minutes = (total % 3600) / 60;
        if (hours == 0)
        {
            return sign + std::to_string(minutes) + "m";
        }
        if (minutes == 0)
        {
            return sign + std::to_string(hours) + "h";
        }
        return sign + std::to_string(hours) + "h " + std::to_string(minutes) + "m";
    }

    Ts day_start(Ts ts)
    {
        std::tm parts = local_tm(ts);
        parts.tm_hour = 0;
        parts.tm_min = 0;
        parts.tm_sec = 0;
        // Falls back to the input if mktime fails
        return to_ts(parts).value_or(ts);
    }

    std::optional<Ts> parse_time(std::string_view text, Ts reference_now)
    {
        const std::string s = lower(trim(text));
        if (s == "now")
        {
            return reference_now;
        }
        // Every remaining shape starts with a digit, which also rejects a sign
        if (s.empty() || !is_digit(s.front()))
        {
            return std::nullopt;
        }

        int y = 0, mo = 0, d = 0, h = 0, mi = 0, consumed = -1;
        if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d%n", &y, &mo, &d, &h, &mi, &consumed) == 5 &&
            consumed == static_cast<int>(s.size()))
        {
            return from_parts(y, mo, d, h, mi);
        }
        if (parse_date(s, y, mo, d))
        {
            return from_parts(y, mo, d, 0, 0);
        }

        // Clock time: strip am/pm, remainder is "h" or "h:mm"
        std::string body = s;
        const bool twelve = body.size() > 2 &&
                            (body.compare(body.size() - 2, 2, "am") == 0 ||
                             body.compare(body.size() - 2, 2, "pm") == 0);
        bool pm = false;
        if (twelve)
        {
            pm = body[body.size() - 2] == 'p';
            body = std::string(trim(std::string_view(body).substr(0, body.size() - 2)));
        }

        consumed = -1;
        if (std::sscanf(body.c_str(), "%d:%d%n", &h, &mi, &consumed) != 2 ||
            consumed != static_cast<int>(body.size()))
        {
            // Bare hour ("2pm") only in the 12-hour shape
            mi = 0;
            consumed = -1;
            if (!twelve || std::sscanf(body.c_str(), "%d%n", &h, &consumed) != 1 ||
                consumed != static_cast<int>(body.size()))
            {
                return std::nullopt;
            }
        }

        if (twelve)
        {
            if (h < 1 || h > 12)
            {
                return std::nullopt;
            }
            h = h % 12 + (pm ? 12 : 0);
        }

        const std::tm day = local_tm(reference_now);
        return from_parts(day.tm_year + 1900, day.tm_mon + 1, day.tm_mday, h, mi);
    }

    std::optional<Dur> parse_duration(std::string_view text)
    {
        const std::string s = lower(trim(text));
        double total = 0.0;
        int pairs = 0;

        for (std::size_t i = 0; i < s.size();)
        {
            if (s[i] == ' ' || s[i] == '\t')
            {
                ++i;
                continue;
            }

            const std::size_t start = i;
            const std::size_t int_digits = scan_digits(s, i);
            if (i < s.size() && s[i] == '.')
            {
                ++i;
                scan_digits(s, i);
            }
            // Leading digit required => strtod never sees "inf", a sign, or a bare "."
            if (int_digits == 0)
            {
                return std::nullopt;
            }
            if (i >= s.size())
            {
                return std::nullopt; // trailing number with no unit
            }

            double scale = 0.0;
            switch (s[i])
            {
            case 'h':
                scale = 3600.0;
                break;
            case 'm':
                scale = 60.0;
                break;
            case 's':
                scale = 1.0;
                break;
            default:
                return std::nullopt;
            }
            ++i;
            ++pairs;
            total += std::strtod(s.substr(start, i - start - 1).c_str(), nullptr) * scale;
        }

        if (pairs == 0 || total > 1e15)
        {
            return std::nullopt;
        }
        return Dur{static_cast<std::int64_t>(std::llround(total))};
    }

    std::optional<Ts> parse_since(std::string_view text, Ts reference_now)
    {
        const std::string s = lower(trim(text));

        // "week" and "month" are rolling 7 and 30 days, not calendar windows
        if (s == "today")
        {
            return day_start(reference_now);
        }
        if (s == "yesterday")
        {
            // Steps tm_mday, not day_start() - 24h: after a DST shift that lands at 23:00 two days back
            std::tm parts = local_tm(reference_now);
            parts.tm_mday -= 1;
            parts.tm_hour = 0;
            parts.tm_min = 0;
            parts.tm_sec = 0;
            return to_ts(parts);
        }
        if (s == "week")
        {
            return reference_now - std::chrono::days{7};
        }
        if (s == "month")
        {
            return reference_now - std::chrono::days{30};
        }

        long long n = 0;
        int consumed = -1;
        if (s.size() >= 2 && is_digit(s.front()) &&
            std::sscanf(s.c_str(), "%lld%n", &n, &consumed) == 1 &&
            consumed == static_cast<int>(s.size()) - 1 && n <= 1000000)
        {
            if (s.back() == 'd')
            {
                return reference_now - Dur{n * 86400};
            }
            if (s.back() == 'h')
            {
                return reference_now - Dur{n * 3600};
            }
            return std::nullopt;
        }

        int y = 0, mo = 0, d = 0;
        if (parse_date(s, y, mo, d))
        {
            return parse_time(s, reference_now);
        }
        return std::nullopt;
    }

}
