#pragma once

// Reads a client's Outside Counsel Guidelines and reports what they say, with the
// section. Decides nothing. No database, no model, no clock of its own.

#include "tt/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tt
{

    inline constexpr std::string_view kTimekeeper = "timekeeper_attendance";
    inline constexpr std::string_view kExpert = "expert_retention";
    inline constexpr std::string_view kTravel = "travel_time";
    inline constexpr std::string_view kExpense = "expense_threshold";

    enum class Finding
    {
        // Nothing in this client's guidelines applies.
        NoRestrictionFound,
        // Rule applies, record already meets it
        Satisfied,
        // Rule applies, something outstanding
        ActionRequired,
        // Need more from the lawyer: See Result::missing
        NeedsInformation,
    };

    inline std::string_view to_string(Finding finding)
    {
        switch (finding)
        {
        case Finding::NoRestrictionFound:
            return "no_restriction_found";
        case Finding::Satisfied:
            return "satisfied";
        case Finding::ActionRequired:
            return "action_required";
        case Finding::NeedsInformation:
            return "needs_information";
        }
        return "unknown";
    }

    // Where an answer came from.
    struct Citation
    {
        std::string document{};
        std::string section{};
        std::string excerpt{};
        int page{};
    };

    // threshold_cents for expenses.
    struct Rule
    {
        std::string client{};
        std::string category{};
        // Seed text, reported not branched on. The category decides the logic.
        std::string effect{};
        std::string approval_role{};
        double rate_multiplier = 1.0;
        std::int64_t threshold_cents = 0;
        bool threshold_inclusive = true;
        Citation citation{};
    };

    struct Person
    {
        std::string name{};
        std::string role{};
        std::string email{};
        std::string client{};
        std::vector<std::string> authorized{};
    };

    struct Approval
    {
        std::string matter{};
        std::string category{};
        std::string person{};
        std::string status{};
        std::optional<Ts> decided_at{};
    };

    // The question
    struct ProposedWork
    {
        std::string client{};
        std::string matter{};
        std::string category{};
        // Empty when the lawyer has not said who yet.
        std::string person{};
        std::optional<double> hours{};
        std::optional<std::int64_t> amount_cents{};
        // When the work would happen. Approvals must predate it.
        Ts at{};
    };

    struct Result
    {
        Finding finding{};
        std::string summary{};
        std::optional<Citation> citation{};
        std::optional<double> rate_multiplier{};
        std::optional<std::string> approval_on_file{};
        std::vector<std::string> missing{};
    };

    struct Draft
    {
        std::string to{};
        std::string email{};
        std::string subject{};
        std::string body{};
        bool sent = false;
    };

    class Compliance
    {
    public:
        // `anchor` = local midnight the seed's relative days check against
        static Compliance from_file(const std::string &path, Ts anchor);

        static Compliance from_json(std::string_view text, Ts anchor);

        // Never throws for a question it can't answer => NeedsInformation.
        [[nodiscard]] Result check(const ProposedWork &work) const;

        // Pass back what check() returned
        [[nodiscard]] Draft draft(const ProposedWork &work, const Result &result) const;

    private:
        // First match, or null
        [[nodiscard]] const Rule *ruleFor(std::string_view client,
                                          std::string_view category) const;
        [[nodiscard]] const Person *person(std::string_view name) const;
        // Client-side recipient for `rule`, or null
        [[nodiscard]] const Person *approver(const Rule &rule,
                                             std::string_view client) const;
        // Matching approval, any status
        [[nodiscard]] const Approval *approvalFor(const ProposedWork &work) const;

        std::vector<Rule> rules_{};
        std::vector<Person> people_{};
        std::vector<Approval> approvals_{};
    };

}
