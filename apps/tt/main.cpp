// The one executable. Paths are relative: run from the repository root.

#include <tt/agent.hpp>
#include <tt/json.hpp>
#include <tt/timeutil.hpp>
#include <tt/tracker.hpp>

#ifdef TT_VOICE
#include <tt/voice.hpp>
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

    // 2 is reserved for "the name was not unique", a question and not a failure.
    constexpr int kOk = 0;
    constexpr int kError = 1;
    constexpr int kAmbiguous = 2;

    void usage()
    {
        std::cerr <<
            R"(usage: tt [--db PATH] <command>

  new-client <name>                 create a client
  new-task <client> <name> [desc]   create a matter under a client
  tasks                             every task with its total
  search <phrase>                   what resolve() makes of a phrase
  chat [text]                       talk to the model; no text means a REPL
  voice                             press Enter, talk, press Enter, hear it back

The database defaults to $TT_DB, or ./tt.db.
$TT_PROMPT and $TT_TOOLS override prompts/system.md and prompts/tools.json,
which is how you try a prompt change without rebuilding.
)";
    }

    // --db wins over this; main strips it from `args` before dispatching.
    std::string default_db()
    {
        if (const char *from_env = std::getenv("TT_DB"))
        {
            return from_env;
        }
        return "tt.db";
    }

    // Prints the candidates and exits rather than picking one.
    tt::Id resolve_or_exit(const tt::Tracker &tracker, const std::string &phrase,
                           tt::EntityKind kind)
    {
        const tt::Resolution r = tracker.resolve(phrase, kind);

        if (r.verdict == tt::ResolveVerdict::Unique)
        {
            return r.candidates.front().id;
        }

        if (r.candidates.empty())
        {
            std::cerr << "nothing matches '" << phrase << "'\n";
        }
        else
        {
            std::cerr << "'" << phrase << "' is not specific enough:\n";
            for (const tt::Candidate &c : r.candidates)
            {
                std::cerr << "  " << c.id << "  " << c.name;
                if (c.kind == tt::EntityKind::Task)
                {
                    std::cerr << "  (" << c.client_name << ")";
                }
                std::cerr << "  score " << c.score << "\n";
            }
        }
        std::exit(kAmbiguous);
    }

    void print_tasks(const tt::Tracker &tracker)
    {
        for (const tt::TaskInfo &t : tracker.find())
        {
            std::cout << t.task.id << "  " << t.client_name << " / " << t.task.name
                      << "  " << tt::format_duration(t.total_all_time)
                      << (t.running ? "  [running]" : "")
                      << (t.task.finished ? "  [done]" : "") << "\n";
        }
    }

    // Strips what a screen shows but a speaker should not say.
    // Example: "Recorded **3 hours** to Northgate. ✅" => "Recorded 3 hours to Northgate."
    // Kokoro pronounces the asterisks and the tick mark. Spoken path only.
    std::string for_speech(const std::string &text)
    {
        std::string out;
        out.reserve(text.size());

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);

            if (c == '*' || c == '#' || c == '`')
            {
                continue; // markdown emphasis, headings, code ticks
            }
            if (c >= 0xE2) // symbols, arrows, ticks and emoji: 3- and 4-byte UTF-8
            {              // accented Latin starts at 0xC2, left alone
                const std::size_t width = (c >= 0xF0) ? 4 : 3;
                i += width - 1;
                out.push_back(' ');
                continue;
            }
            out.push_back(text[i]);
        }

        // Removing a run of symbols leaves doubled spaces behind.
        std::string tidy;
        tidy.reserve(out.size());
        for (const char c : out)
        {
            if (c == ' ' && !tidy.empty() && tidy.back() == ' ')
            {
                continue;
            }
            tidy.push_back(c);
        }
        return tidy;
    }

    // `tt chat` passes stdin and stdout, `tt voice` the microphone and speaker.
    void converse(tt::Agent &agent,
                  const std::function<std::string()> &read,
                  const std::function<void(const std::string &)> &write)
    {
        while (true)
        {
            const std::string heard = read();

            // Both modes read stdin: chat the line, voice the Enter that ends a recording.
            // Closed stdin would spin the microphone forever. Before the empty case, a retry.
            if (!std::cin)
            {
                return;
            }
            if (heard.empty())
            {
                continue;
            }
            if (heard == "quit" || heard == "exit")
            {
                return;
            }
            try
            {
                write(agent.say(heard));
            }
            catch (const std::exception &e)
            {
                // Losing Ollama is the only failure say() lets escape. Keep the loop alive.
                std::cerr << "error: " << e.what() << "\n";
            }
        }
    }

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string db = default_db();
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
    {
        if (args[i] == "--db")
        {
            db = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            break;
        }
    }

    if (args.empty())
    {
        usage();
        return kError;
    }

    const std::string command = args[0];

    try
    {
        tt::Tracker tracker(db);

        if (command == "new-client" && args.size() >= 2)
        {
            const tt::Client c = tracker.createClient(args[1]);
            std::cout << c.id << "  " << c.name << "\n";
            return kOk;
        }

        if (command == "new-task" && args.size() >= 3)
        {
            const tt::Id client = resolve_or_exit(tracker, args[1], tt::EntityKind::Client);
            std::optional<std::string> description;
            if (args.size() >= 4)
            {
                description = args[3];
            }
            const tt::Task t = tracker.createTask(client, args[2], description);
            std::cout << t.id << "  " << t.name << "\n";
            return kOk;
        }

        if (command == "tasks")
        {
            print_tasks(tracker);
            return kOk;
        }

        if (command == "search" && args.size() >= 2)
        {
            const tt::Resolution r = tracker.resolve(args[1]);
            std::cout << to_string(r.verdict) << "\n";
            for (const tt::Candidate &c : r.candidates)
            {
                std::cout << "  " << to_string(c.kind) << "  " << c.id << "  " << c.name
                          << "  score " << c.score << (c.exact ? "  exact" : "") << "\n";
            }
            return kOk;
        }

        if (command == "chat")
        {
            auto session = tracker.session("chat");
            tt::Agent agent(tracker, tt::options_from_env());

            if (args.size() >= 2) // one shot
            {
                std::cout << agent.say(args[1]) << "\n";
                return kOk;
            }

            std::cout << "Type to the assistant. 'quit' to stop.\n";
            converse(
                agent,
                []
                {
                    std::cout << "> " << std::flush;
                    std::string line;
                    if (!std::getline(std::cin, line))
                    {
                        return std::string("quit");
                    }
                    return line;
                },
                [](const std::string &reply)
                { std::cout << reply << "\n"; });
            return kOk;
        }

        if (command == "voice")
        {
#ifdef TT_VOICE
            auto session = tracker.session("voice");
            tt::Agent agent(tracker, tt::options_from_env());
            tt::Voice voice{tt::VoiceOptions{}};

            converse(
                agent,
                [&voice]
                { return voice.listen(); },
                [&voice](const std::string &reply)
                {
                    std::cout << reply << "\n";
                    voice.speak(for_speech(reply));
                });
            return kOk;
#else
            std::cerr << "voice support was not compiled in; "
                         "reconfigure with -DTT_ENABLE_VOICE=ON\n";
            return kError;
#endif
        }

        usage();
        return kError;
    }
    catch (const std::exception &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return kError;
    }
}
