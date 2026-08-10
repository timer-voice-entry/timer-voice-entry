#include <tt/agent.hpp>

#include <tt/json.hpp>
#include <tt/timeutil.hpp>

#include <httplib.h>
#include <string>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace tt
{
    namespace
    {

        // A JSON integer, or a numeric string Invalid otherwise
        Id require_id(const nlohmann::json &args, const char *key)
        {
            if (!args.contains(key) || args.at(key).is_null())
            {
                throw Invalid(std::string("missing ") + key);
            }
            const nlohmann::json &value = args.at(key);
            if (value.is_number_integer())
            {
                return value.get<Id>();
            }
            if (value.is_string())
            {
                const std::string str_id = value.get<std::string>();
                Id id = std::stoll(str_id);
                return id;
            }
            // dump() puts what the model sent into the error it reads back
            throw Invalid(std::string(key) + " is not an id: " + value.dump());
        }

        // As require_id, and rejects zero or negative
        Dur require_seconds(const nlohmann::json &args, const char *key)
        {
            if (!args.contains(key) || args.at(key).is_null())
            {
                throw Invalid(std::string("missing ") + key);
            }
            const nlohmann::json &value = args.at(key);
            if (value.is_number_integer())
            {
                const std::int64_t seconds = value.get<std::int64_t>();
                if (seconds <= 0)
                {
                    throw Invalid(std::string(key) + " must be positive: " + value.dump());
                }
                return Dur{seconds};
            }
            if (value.is_string())
            {
                const std::string str_seconds = value.get<std::string>();
                const std::int64_t seconds = std::stoll(str_seconds);
                if (seconds <= 0)
                {
                    throw Invalid(std::string(key) + " must be positive: " + value.dump());
                }
                return Dur{seconds};
            }
            // dump() puts what the model sent into the error it reads back
            throw Invalid(std::string(key) + " is not a duration: " + value.dump());
        }

        // The value as text; nullopt when absent or empty
        std::optional<std::string> optional_text(const nlohmann::json &args, const char *key)
        {
            if (!args.contains(key) || args.at(key).is_null())
            {
                return std::nullopt;
            }
            const nlohmann::json &value = args.at(key);
            if (value.is_string())
            {
                const std::string text = value.get<std::string>();
                if (text.empty())
                {
                    return std::nullopt;
                }
                return text;
            }

            // Keep whatever it sent; a note is free text either way
            return value.dump();
        }

        // A number if the model sent one, whatever spelling it used.
        std::optional<double> optional_number(const nlohmann::json &args, const char *key)
        {
            if (!args.contains(key) || args.at(key).is_null())
            {
                return std::nullopt;
            }
            const nlohmann::json &value = args.at(key);
            if (value.is_number())
            {
                return value.get<double>();
            }
            if (value.is_string())
            {
                // "5" and "five hours" both arrive here; only the first survives.
                try
                {
                    return std::stod(value.get<std::string>());
                }
                catch (const std::exception &)
                {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        // Client and matter names come off the task, not off the model.
        ProposedWork work_from(const nlohmann::json &args, const Tracker &tracker)
        {
            const Id matter = require_id(args, "matter_id");
            const std::optional<TaskInfo> info = tracker.getTask(matter);
            if (!info)
            {
                throw Invalid("no matter with id " + std::to_string(matter));
            }

            ProposedWork work;
            work.client = info->client_name;
            work.matter = info->task.name;
            work.category = args.value("category", "");
            work.person = optional_text(args, "person").value_or("");
            work.hours = optional_number(args, "hours");

            if (const std::optional<double> amount = optional_number(args, "amount_dollars"))
            {
                work.amount_cents = static_cast<std::int64_t>(*amount * 100);
            }

            // 9am on the day asked => "tomorrow's deposition" is sayable
            const int days = static_cast<int>(optional_number(args, "days_ahead").value_or(0.0));
            work.at = day_start(tracker.now()) + std::chrono::hours(9 + 24 * days);
            return work;
        }

        // Only the fields the model can act on.
        nlohmann::json result_json(const Result &result)
        {
            nlohmann::json out;
            out["finding"] = to_string(result.finding);
            out["summary"] = result.summary;
            out["missing"] = result.missing;

            if (result.rate_multiplier)
            {
                out["rate_multiplier"] = *result.rate_multiplier;
            }
            if (result.approval_on_file)
            {
                out["approval_on_file"] = *result.approval_on_file;
            }
            if (result.citation)
            {
                out["citation"] = {{"document", result.citation->document},
                                   {"section", result.citation->section},
                                   {"page", result.citation->page},
                                   {"excerpt", result.citation->excerpt}};
            }
            return out;
        }

        nlohmann::json draft_json(const Draft &draft)
        {
            return {{"to", draft.to},
                    {"email", draft.email},
                    {"subject", draft.subject},
                    {"body", draft.body},
                    {"sent", draft.sent}}; // always false, and said out loud
        }

        // Tool schemas from `path`. The descriptions are prompt text.
        nlohmann::json tool_schemas(const std::string &path)
        {
            std::ifstream file(path);
            if (!file)
            {
                throw std::runtime_error("tool_schemas: cannot open " + path);
            }
            return nlohmann::json::parse(file);
        }

        // Reads the prompt file and substitutes {{TODAY}}.
        std::string load_prompt(const std::string &path, Ts now)
        {
            std::ifstream file(path);
            if (!file)
            {
                throw std::runtime_error("load_prompt: cannot open " + path);
            }
            std::string text((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

            const std::string marker = "{{TODAY}}";
            const std::string today = format_local(now).substr(0, 10);
            std::size_t at = text.find(marker);
            while (at != std::string::npos)
            {
                text.replace(at, marker.size(), today);
                at = text.find(marker, at + today.size());
            }
            return text;
        }

    } // namespace

    AgentOptions options_from_env()
    {
        AgentOptions options;

        if (const char *model = std::getenv("TT_MODEL"))
        {
            options.model = model;
        }

        // The prompt files make this a timekeeper vs a preflight assistant
        if (const char *prompt = std::getenv("TT_PROMPT"))
        {
            options.prompt_path = prompt;
        }

        if (const char *tools = std::getenv("TT_TOOLS"))
        {
            options.tools_path = tools;
        }

        if (const char *endpoint = std::getenv("TT_OLLAMA_HOST"))
        {
            const std::string value = endpoint;
            const std::size_t colon = value.rfind(':');
            if (colon == std::string::npos)
            {
                options.host = value;
            }
            else
            {
                options.host = value.substr(0, colon);
                options.port = std::stoi(value.substr(colon + 1));
            }
        }

        return options;
    }

    Agent::Agent(Tracker &tracker, AgentOptions options)
        : tracker_(tracker), options_(std::move(options))
    {
        tools_ = tool_schemas(options_.tools_path);
        messages_ = nlohmann::json::array();
        messages_.push_back({{"role", "system"},
                             {"content", load_prompt(options_.prompt_path, tracker.now())}});
    }

    // Also gives up if the model repeats a call identically, Rest of the contract in agent.hpp.
    std::string Agent::say(std::string_view speech)
    {
        messages_.push_back({{"role", "user"}, {"content", std::string(speech)}});

        // This turn only, The window reads it once say() returns.
        last_results_ = nlohmann::json::array();

        int rounds = 0;
        int calls = 0;
        bool nudged = false;
        bool wrote = false;
        std::string last;

        while (true)
        {
            const nlohmann::json reply = chat();

            // qwen3.5 puts its reasoning in `thinking`
            nlohmann::json stored = reply;
            stored.erase("thinking");
            messages_.push_back(stored);

            if (!reply.contains("tool_calls") || reply["tool_calls"].empty())
            {
                const std::string content = reply.value("content", "");
                if (content.empty())
                {
                    // Only thinking came back, usually right on a second pass
                    if (!nudged)
                    {
                        nudged = true;
                        messages_.push_back({{"role", "user"},
                                             {"content", "Answer in one short sentence."}});
                        continue;
                    }
                    return "Sorry, I lost my train of thought there. Say that again?";
                }
                // Read the totals back from the rows; the model lies about writes
                return wrote ? content + " " + verified_today() : content;
            }

            for (const auto &tool_call : reply["tool_calls"])
            {
                const std::string name = tool_call["function"]["name"].get<std::string>();
                const nlohmann::json &args = tool_call["function"]["arguments"];
                const std::string key = name + args.dump();

                nlohmann::json result;
                if (++calls > options_.max_calls)
                {
                    result = {{"error", "too many tool calls in one turn"}};
                }
                else if (key == last)
                {
                    result = {{"error", "identical call repeated; try something else"}};
                }
                else
                {
                    result = dispatch(name, args);
                    // Any tool that can change a row => append the read-back at the end of the turn
                    wrote = wrote || name == "log_time" || name == "create_task" ||
                            name == "start_timer" || name == "stop_timer" ||
                            name == "create_client" || name == "delete_interval" ||
                            name == "set_interval_duration";
                }
                last = key;
                last_results_.push_back({{"tool", name}, {"result", result}});

                // Ollama requires exactly one tool result per tool call
                messages_.push_back({{"role", "tool"},
                                     {"tool_name", name},
                                     {"content", result.dump()}});
            }

            if (++rounds >= options_.max_rounds)
            {
                return "I got stuck working on that.";
            }
        }
    }

    // Throws if Ollama is unreachable
    nlohmann::json Agent::chat()
    {
        httplib::Client cli(options_.host, options_.port);
        // Long read timeout, inference is slow; short connect timeout, an absent Ollama fails now
        cli.set_read_timeout(300, 0);
        cli.set_connection_timeout(5, 0);

        nlohmann::json body;
        body["model"] = options_.model;
        body["messages"] = messages_;
        body["tools"] = tools_;
        body["stream"] = false;

        auto res = cli.Post("/api/chat", body.dump(), "application/json");
        if (!res)
        {
            throw std::runtime_error("ollama unreachable: " + httplib::to_string(res.error()));
        }
        if (res->status != 200)
        {
            throw std::runtime_error("ollama returned " + std::to_string(res->status) + ": " + res->body);
        }
        const nlohmann::json response = nlohmann::json::parse(res->body);
        return response.at("message");
    }

    // Read back from Tracker, not from the model
    std::string Agent::verified_today()
    {
        const Ts day = day_start(tracker_.now());
        const Range window{.from = day, .to = day + std::chrono::hours{24}};

        std::string entries;
        Dur total{0};
        for (const TaskInfo &t : tracker_.find({.range = window}))
        {
            if (t.total_in_range.count() == 0)
            {
                continue;
            }
            total += t.total_in_range;
            if (!entries.empty())
            {
                entries += ", ";
            }
            entries += t.client_name + " " + t.task.name + " " +
                       format_duration(t.total_in_range);
        }

        if (entries.empty())
        {
            return "Nothing is recorded for today.";
        }
        return "Recorded today: " + entries + ". Total " + format_duration(total) + ".";
    }

    const Compliance &Agent::compliance()
    {
        if (!compliance_)
        {
            // Same clock work_from() uses
            compliance_ = Compliance::from_file(options_.compliance_path,
                                                day_start(tracker_.now()));
        }
        return *compliance_;
    }

    // Failures come back as {"error": ...} instead of throwing
    nlohmann::json Agent::dispatch(const std::string &name, const nlohmann::json &args)
    {
        try
        {
            if (name == "check_rules")
            {
                return result_json(compliance().check(work_from(args, tracker_)));
            }
            if (name == "draft_approval")
            {
                // Re-checked, not carried over: the model drops a rule id held across two turns
                const ProposedWork work = work_from(args, tracker_);
                const Result result = compliance().check(work);
                return draft_json(compliance().draft(work, result));
            }
            if (name == "resolve")
            {
                const std::string phrase = args.at("phrase").get<std::string>();

                std::optional<EntityKind> kind;
                if (args.contains("kind") && args.at("kind").is_string())
                {
                    const std::string text = args.at("kind").get<std::string>();
                    if (text == "client")
                    {
                        kind = EntityKind::Client;
                    }
                    else if (text == "task")
                    {
                        kind = EntityKind::Task;
                    }
                    // anything else leaves it empty => a bad enum searches both
                }

                return tracker_.resolve(phrase, kind);
            }
            if (name == "create_task")
            {
                const Id client = require_id(args, "client_id");
                const std::string task_name = args.at("name").get<std::string>();

                return tracker_.createTask(client, task_name, optional_text(args, "description"));
            }
            if (name == "log_time")
            {
                const Id task = require_id(args, "task_id");
                const Dur seconds = require_seconds(args, "seconds");
                const Interval interval = tracker_.addTime(task, seconds, optional_text(args, "note"));

                nlohmann::json out;
                out["interval"] = interval;
                out["task_total_seconds"] = tracker_.getTask(task)->total_all_time.count();
                return out;
            }
            if (name == "start_timer")
            {
                return tracker_.startWork(require_id(args, "task_id"));
            }
            if (name == "stop_timer")
            {
                // No task named => stop whatever is running
                if (!args.contains("task_id") || args.at("task_id").is_null())
                {
                    nlohmann::json out;
                    out["stopped"] = tracker_.stopAll();
                    return out;
                }

                const Id task = require_id(args, "task_id");
                const Interval interval = tracker_.stopWork(task);

                // Seconds and the new total come from here, the model gets the duration wrong.
                nlohmann::json out;
                out["interval"] = interval;
                out["seconds"] = (*interval.end - interval.start).count();
                out["task_total_seconds"] = tracker_.getTask(task)->total_all_time.count();
                return out;
            }
            if (name == "review_day")
            {
                const std::string date = args.at("date").get<std::string>();
                const auto day = parse_time(date, tracker_.now());
                if (!day)
                {
                    return {{"error", "cannot read the date '" + date + "'"}};
                }
                const Range window{.from = *day, .to = *day + std::chrono::hours{24}};

                nlohmann::json entries = nlohmann::json::array();
                Dur total{0};
                for (const TaskInfo &t : tracker_.find({.range = window}))
                {
                    const auto detail = tracker_.getTask(t.task.id, window);
                    if (!detail)
                    {
                        continue;
                    }
                    for (const Interval &iv : detail->intervals)
                    {
                        if (iv.kind != IntervalKind::Work)
                        {
                            continue;
                        }
                        const Dur seconds = (iv.end ? *iv.end : tracker_.now()) - iv.start;
                        total += seconds;
                        entries.push_back({{"interval_id", iv.id},
                                           {"task_id", t.task.id},
                                           {"task_name", t.task.name},
                                           {"client_name", t.client_name},
                                           {"start", to_epoch(iv.start)},
                                           {"seconds", seconds.count()}});
                    }
                }

                return {{"date", date}, {"entries", entries}, {"total_seconds", total.count()}};
            }
            if (name == "set_interval_duration")
            {
                const Id interval = require_id(args, "interval_id");
                const Dur seconds = require_seconds(args, "seconds");
                const Interval updated = tracker_.setIntervalDuration(interval, seconds);

                nlohmann::json out;
                out["interval"] = updated;
                out["task_total_seconds"] = tracker_.getTask(updated.task_id)->total_all_time.count();
                return out;
            }
            if (name == "delete_interval")
            {
                const Id task = require_id(args, "task_id");
                const Id interval = require_id(args, "interval_id");
                tracker_.deleteInterval(interval);

                nlohmann::json out;
                out["deleted"] = true;
                // deleteInterval ignores task_id, guard it here. In log_time the write validates it.
                if (const auto info = tracker_.getTask(task))
                {
                    out["task_total_seconds"] = info->total_all_time.count();
                }
                return out;
            }

            if (name == "list_tasks")
            {
                // Only use review_day not intervals
                Query query;
                if (args.contains("client_id"))
                {
                    query.client = require_id(args, "client_id");
                }

                nlohmann::json tasks = nlohmann::json::array();
                for (const TaskInfo &t : tracker_.find(query))
                {
                    tasks.push_back({{"task_id", t.task.id},
                                     {"task_name", t.task.name},
                                     {"client_name", t.client_name},
                                     {"total_seconds", t.total_all_time.count()},
                                     {"running", t.running},
                                     {"finished", t.task.finished}});
                }
                return {{"tasks", tasks}};
            }
            if (name == "create_client")
            {
                return tracker_.createClient(args.at("name").get<std::string>());
            }

            return {{"error", "unknown tool '" + name + "'"}};
        }
        catch (const std::exception &e)
        {
            return {{"error", e.what()}};
        }
    }
}
