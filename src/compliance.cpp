#include <tt/compliance.hpp>

#include <tt/errors.hpp>
#include <tt/match.hpp>
#include <tt/timeutil.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace tt
{
    namespace
    {

        using nlohmann::json;

        // normalize through whisper and model
        bool same(std::string_view a, std::string_view b) { return exact_match(a, b); }

        // Example: "$1250.50" for a cent count
        std::string money(std::int64_t cents)
        {
            std::ostringstream out;
            out << '$' << cents / 100;
            if (cents % 100 != 0)
            {
                out << '.' << (cents % 100) / 10 << (cents % 100) % 10;
            }
            return out.str();
        }

        std::string date_only(Ts ts) { return format_local(ts).substr(0, 10); }

        // Approval only counts if it was decided before the work.
        bool approved_before(const Approval *approval, Ts at)
        {
            return approval != nullptr && approval->status == "approved" &&
                   approval->decided_at.has_value() && *approval->decided_at < at;
        }

        std::optional<std::string> on_file(const Approval *approval)
        {
            if (approval == nullptr)
            {
                return std::nullopt;
            }
            if (approval->status == "approved" && approval->decided_at)
            {
                return "approved " + date_only(*approval->decided_at);
            }
            if (approval->decided_at)
            {
                return approval->status + " " + date_only(*approval->decided_at);
            }
            return approval->status; // pending, with nothing decided yet
        }

        // Every substantive answer quotes its section.
        std::string cite(const Rule &rule) { return rule.citation.section; }

        // ActionRequired => Asks for something

        Result timekeeper(const ProposedWork &work, const Rule &rule, const Person *who,
                          const Approval *approval)
        {
            Result out;
            out.citation = rule.citation;
            out.approval_on_file = on_file(approval);

            if (work.person.empty() || who == nullptr)
            {
                out.finding = Finding::NeedsInformation;
                out.missing = {"person"};
                out.summary = cite(rule) + " covers who may work on this matter. Who would that be?";
                return out;
            }

            for (const std::string &matter : who->authorized)
            {
                if (same(matter, work.matter))
                {
                    out.finding = Finding::Satisfied;
                    out.summary = cite(rule) + " requires client-approved timekeepers. " +
                                  who->name + " is on the approved list for " + work.matter + ".";
                    return out;
                }
            }

            if (approved_before(approval, work.at))
            {
                out.finding = Finding::Satisfied;
                out.summary = cite(rule) + " requires client-approved timekeepers. " + who->name +
                              " is not on the staffing list, but there is an authorisation on file " +
                              *out.approval_on_file + ".";
                return out;
            }

            out.finding = Finding::ActionRequired;
            out.summary = cite(rule) + " asks for written authorisation before an unlisted timekeeper "
                                       "works. " +
                          who->name + " is not on the approved list for " + work.matter;
            // An approval decided after the work looks like coverage and is not, so say that part out loud.
            if (approval != nullptr && approval->status == "approved" && approval->decided_at &&
                *approval->decided_at >= work.at)
            {
                out.summary += ", and the authorisation on file was decided " +
                               date_only(*approval->decided_at) + ", after the work would start.";
            }
            else if (out.approval_on_file)
            {
                out.summary += ", and the request on file is " + *out.approval_on_file + ".";
            }
            else
            {
                out.summary += ", and I don't see an authorisation on file.";
            }
            return out;
        }

        Result expert(const ProposedWork &work, const Rule &rule, const Approval *approval)
        {
            Result out;
            out.citation = rule.citation;
            out.approval_on_file = on_file(approval);

            if (work.person.empty())
            {
                out.finding = Finding::NeedsInformation;
                out.missing = {"expert"};
                out.summary = cite(rule) + " covers engaging experts. Which expert did you have in mind?";
                return out;
            }

            if (approved_before(approval, work.at))
            {
                out.finding = Finding::Satisfied;
                out.summary = cite(rule) + " requires approval before engaging an expert, and " +
                              work.person + " was " + *out.approval_on_file + ".";
                return out;
            }

            out.finding = Finding::ActionRequired;
            out.summary = cite(rule) + " asks for approval before an expert is engaged";
            out.summary += out.approval_on_file
                               ? ". The request for " + work.person + " is " + *out.approval_on_file + "."
                               : ", and I don't see one on file for " + work.person + ".";
            return out;
        }

        Result travel(const ProposedWork &work, const Rule &rule)
        {
            Result out;
            out.finding = Finding::Satisfied;
            out.citation = rule.citation;
            out.rate_multiplier = rule.rate_multiplier;

            out.summary = cite(rule) + " puts travel time at " +
                          std::to_string(static_cast<int>(rule.rate_multiplier * 100)) +
                          "% of the standard rate";
            if (work.hours)
            {
                std::ostringstream billed;
                billed.precision(1);
                billed << std::fixed << (*work.hours * rule.rate_multiplier);
                out.summary += ", so " + std::to_string(static_cast<int>(*work.hours)) +
                               " hours would bill as " + billed.str() + ".";
            }
            else
            {
                out.summary += ".";
            }
            return out;
        }

        Result expense(const ProposedWork &work, const Rule &rule)
        {
            Result out;
            out.citation = rule.citation;

            if (!work.amount_cents)
            {
                out.finding = Finding::NeedsInformation;
                out.missing = {"amount"};
                out.summary = cite(rule) + " sets a threshold of " + money(rule.threshold_cents) +
                              ". How much is the expense?";
                return out;
            }

            // "$500 or more" catches the threshold itself vs "exceeding $1,000" does
            const bool over = rule.threshold_inclusive ? *work.amount_cents >= rule.threshold_cents
                                                       : *work.amount_cents > rule.threshold_cents;
            const std::string bound =
                (rule.threshold_inclusive ? " at or above " : " above ") + money(rule.threshold_cents);

            if (!over)
            {
                out.finding = Finding::Satisfied;
                out.summary = cite(rule) + " requires approval for expenses" + bound + ", and " +
                              money(*work.amount_cents) + " is under that.";
                return out;
            }

            out.finding = Finding::ActionRequired;
            out.summary = cite(rule) + " asks for approval for expenses" + bound + ", and this one is " +
                          money(*work.amount_cents) + ".";
            return out;
        }

        Citation read_citation(const json &j)
        {
            Citation c;
            c.document = j.value("document", "");
            c.section = j.value("section", "");
            c.excerpt = j.value("excerpt", "");
            c.page = j.value("page", 0);
            return c;
        }

        Rule read_rule(const json &j)
        {
            Rule r;
            r.client = j.value("client", "");
            r.category = j.value("category", "");
            r.effect = j.value("effect", "");
            r.approval_role = j.value("approval_role", "");
            r.rate_multiplier = j.value("rate_multiplier", 1.0);
            r.threshold_cents = j.value("threshold_cents", std::int64_t{0});
            r.threshold_inclusive = j.value("threshold_inclusive", true);
            r.citation = read_citation(j);
            return r;
        }

        Person read_person(const json &j)
        {
            Person p;
            p.name = j.value("name", "");
            p.role = j.value("role", "");
            p.email = j.value("email", "");
            p.client = j.value("client", "");
            p.authorized = j.value("authorized", std::vector<std::string>{});
            return p;
        }

        Approval read_approval(const json &j, Ts anchor)
        {
            Approval a;
            a.matter = j.value("matter", "");
            a.category = j.value("category", "");
            a.person = j.value("person", "");
            a.status = j.value("status", "pending");
            // Days from the anchor rather than a date, so the seed stays current. A
            // pending approval has no decided_day at all.
            if (j.contains("decided_day") && !j.at("decided_day").is_null())
            {
                a.decided_at = anchor + std::chrono::hours(24 * j.at("decided_day").get<int>());
            }
            return a;
        }

    } // namespace

    Compliance Compliance::from_json(std::string_view text, Ts anchor)
    {
        json doc = json::parse(text, nullptr, false);
        if (doc.is_discarded())
        {
            throw Invalid("compliance seed is not valid JSON");
        }

        Compliance out;
        for (const json &j : doc.value("rules", json::array()))
        {
            out.rules_.push_back(read_rule(j));
        }
        for (const json &j : doc.value("people", json::array()))
        {
            out.people_.push_back(read_person(j));
        }
        for (const json &j : doc.value("approvals", json::array()))
        {
            out.approvals_.push_back(read_approval(j, anchor));
        }
        return out;
    }

    Compliance Compliance::from_file(const std::string &path, Ts anchor)
    {
        std::ifstream file(path);
        if (!file)
        {
            throw Invalid("cannot read compliance seed '" + path + "'");
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return from_json(buffer.str(), anchor);
    }

    const Rule *Compliance::ruleFor(std::string_view client, std::string_view category) const
    {
        for (const Rule &rule : rules_)
        {
            if (same(rule.client, client) && rule.category == category)
            {
                return &rule;
            }
        }
        return nullptr;
    }

    const Person *Compliance::person(std::string_view name) const
    {
        for (const Person &p : people_)
        {
            if (same(p.name, name))
            {
                return &p;
            }
        }
        return nullptr;
    }

    const Person *Compliance::approver(const Rule &rule, std::string_view client) const
    {
        for (const Person &p : people_)
        {
            if (same(p.role, rule.approval_role) && same(p.client, client))
            {
                return &p;
            }
        }
        return nullptr;
    }

    const Approval *Compliance::approvalFor(const ProposedWork &work) const
    {
        for (const Approval &a : approvals_)
        {
            if (same(a.matter, work.matter) && a.category == work.category &&
                same(a.person, work.person))
            {
                return &a;
            }
        }
        return nullptr;
    }

    Result Compliance::check(const ProposedWork &work) const
    {
        const Rule *rule = ruleFor(work.client, work.category);
        if (rule == nullptr)
        {
            Result out;
            out.finding = Finding::NoRestrictionFound;
            out.summary = "I don't see anything in " + work.client + "'s guidelines about " +
                          work.category + ".";
            return out;
        }

        // Both lookups happen once
        const Person *who = person(work.person);
        const Approval *approval = approvalFor(work);

        if (work.category == kTimekeeper)
        {
            return timekeeper(work, *rule, who, approval);
        }
        if (work.category == kExpert)
        {
            return expert(work, *rule, approval);
        }
        if (work.category == kTravel)
        {
            return travel(work, *rule);
        }
        if (work.category == kExpense)
        {
            return expense(work, *rule);
        }

        // Can't evaluate the rule, output that
        Result out;
        out.finding = Finding::NeedsInformation;
        out.citation = rule->citation;
        out.summary = cite(*rule) + " covers this, but I can't evaluate it here. It says: " +
                      rule->citation.excerpt;
        return out;
    }

    Draft Compliance::draft(const ProposedWork &work, const Result &result) const
    {
        const Rule *rule = ruleFor(work.client, work.category);
        const Person *to = (rule != nullptr) ? approver(*rule, work.client) : nullptr;

        Draft d;
        d.to = (to != nullptr) ? to->name : "the client's supervising attorney";
        d.email = (to != nullptr) ? to->email : "";
        d.subject = "Authorisation request -- " + work.matter;

        std::ostringstream body;
        body << d.to << ",\n\n"
             << "I am requesting authorisation on " << work.matter << " (" << work.client << ").\n\n";

        if (!work.person.empty())
        {
            body << "Proposed: " << work.person << "\n";
        }
        if (work.hours)
        {
            body << "Estimated time: " << *work.hours << " hours\n";
        }
        if (work.amount_cents)
        {
            body << "Estimated cost: " << money(*work.amount_cents) << "\n";
        }

        if (result.citation)
        {
            body << "\nUnder " << result.citation->section << " of "
                 << result.citation->document << " (p. " << result.citation->page << "):\n"
                 << "  \"" << result.citation->excerpt << "\"\n";
        }
        body << "\nPlease confirm whether this is authorised.\n";

        d.body = body.str();
        return d; // d.sent stays false
    }

}
