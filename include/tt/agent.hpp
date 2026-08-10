#pragma once

#include "tt/compliance.hpp"
#include "tt/tracker.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace tt
{

    struct AgentOptions
    {
        std::string model = "qwen3.5:4b";
        std::string host = "localhost";
        int port = 11434;
        std::string prompt_path = "prompts/system.md";
        std::string tools_path = "prompts/tools.json";
        // Tool-call rounds per user turn
        int max_rounds = 10;
        // Total tool calls per user turn
        int max_calls = 25;
        // Guidelines, people and approvals
        std::string compliance_path = "data/preflight_seed.json";
    };

    // Defaults with the TT_* environment overrides on top
    AgentOptions options_from_env();

    // One conversation with the model: history, tools, and the loop that runs them
    class Agent
    {
    public:
        Agent(Tracker &tracker, AgentOptions options);

        Agent(const Agent &) = delete;
        Agent &operator=(const Agent &) = delete;

        std::string say(std::string_view utterance);

        [[nodiscard]] const nlohmann::json &history() const { return messages_; }

        [[nodiscard]] const nlohmann::json &last_results() const { return last_results_; }

    private:
        // Opened on first use.
        const Compliance &compliance();

        // One POST to /api/chat. Returns the assistant message.
        nlohmann::json chat();
        // One tool call, result as the JSON the model sees
        nlohmann::json dispatch(const std::string &name, const nlohmann::json &args);
        // Today's entries read straight from the rows, phrased for speech
        std::string verified_today();

        Tracker &tracker_;
        AgentOptions options_;
        nlohmann::json tools_;
        nlohmann::json messages_;
        nlohmann::json last_results_ = nlohmann::json::array();
        std::optional<Compliance> compliance_;
    };

}
