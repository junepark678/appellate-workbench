#include "workflow_action_planner.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/workflow_event.hpp"

#include <QCryptographicHash>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace appellate::app {
namespace {

[[nodiscard]] QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString dispositionName(model::WorkflowOrderDisposition disposition) {
    switch (disposition) {
    case model::WorkflowOrderDisposition::Granted:
        return QStringLiteral("granted");
    case model::WorkflowOrderDisposition::Denied:
        return QStringLiteral("denied");
    case model::WorkflowOrderDisposition::Other:
        return QStringLiteral("other");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] QString legalDateKey(const model::LegalDate& date) {
    return QString::number(std::chrono::sys_days{date.value}.time_since_epoch().count());
}

[[nodiscard]] QString legalDateText(const model::LegalDate& date) {
    return QStringLiteral("%1-%2-%3")
        .arg(static_cast<int>(date.value.year()), 4, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.month()), 2, 10, QLatin1Char('0'))
        .arg(static_cast<unsigned>(date.value.day()), 2, 10, QLatin1Char('0'));
}

[[nodiscard]] QString legalTimeKey(const model::LegalTime& time) {
    return QStringLiteral("%1@%2")
        .arg(time.instant.time_since_epoch().count())
        .arg(legalDateKey(time.court_date));
}

[[nodiscard]] const model::WorkflowCommandHeader&
commandHeader(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowCommandHeader& { return concrete.header; },
        command);
}

[[nodiscard]] model::WorkflowCommandHeader makeHeader(const model::WorkflowState& state,
                                                      const model::CaseActor& actor,
                                                      const model::LegalTime& occurred_at,
                                                      QStringView discriminator) {
    const auto discriminator_bytes = discriminator.toUtf8();
    const auto suffix = QCryptographicHash::hash(discriminator_bytes, QCryptographicHash::Sha256)
                            .toHex()
                            .toStdString();
    return model::WorkflowCommandHeader{
        state.session_id,
        model::WorkflowCommandId{state.session_id + ".command." +
                                 std::to_string(state.next_event_sequence) + "." + suffix},
        actor.id,
        occurred_at,
    };
}

[[nodiscard]] std::vector<const model::CaseActor*>
authorizedActors(const packs::RuntimeCase& runtime_case,
                 const std::vector<model::ActorRoleId>& roles) {
    std::vector<const model::CaseActor*> actors;
    for (const auto& actor : runtime_case.definition.actors) {
        if (std::ranges::contains(roles, actor.role)) {
            actors.push_back(&actor);
        }
    }
    std::ranges::sort(actors, {},
                      [](const auto* actor) { return std::string_view(actor->id.value); });
    return actors;
}

[[nodiscard]] const packs::RuntimeDocketEntry* recordEntry(const packs::RuntimeCase& runtime_case,
                                                           std::string_view entry_id,
                                                           std::string_view digest) {
    const auto found =
        std::ranges::find_if(runtime_case.record.docket_entries, [&](const auto& entry) {
            return entry.id.value == entry_id && entry.asset_sha256 == digest;
        });
    return found == runtime_case.record.docket_entries.end() ? nullptr : &*found;
}

[[nodiscard]] QString actionVerb(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> QString {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return QStringLiteral("File");
            } else if constexpr (std::same_as<Command, model::EnterWorkflowOrder>) {
                return QStringLiteral("Enter order");
            } else if constexpr (std::same_as<Command, model::SetWorkflowSealed>) {
                return concrete.sealed ? QStringLiteral("Seal matter")
                                       : QStringLiteral("Unseal matter");
            } else if constexpr (std::same_as<Command, model::ScheduleWorkflowArgument>) {
                return QStringLiteral("Schedule argument");
            } else if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                return QStringLiteral("Issue judgment");
            } else if constexpr (std::same_as<Command, model::IssueWorkflowMandate>) {
                return QStringLiteral("Issue mandate");
            } else if constexpr (std::same_as<Command, model::CalculateWorkflowDeadline>) {
                return QStringLiteral("Calculate deadline");
            } else {
                return QStringLiteral("Advance stage");
            }
        },
        command);
}

[[nodiscard]] QString actionOperationId(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> QString {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return text(concrete.filing_id.value);
            } else {
                return text(concrete.operation_id.value);
            }
        },
        command);
}

[[nodiscard]] std::optional<std::string>
commandDocumentDigest(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> std::optional<std::string> {
            if constexpr (requires { concrete.document_sha256; }) {
                return concrete.document_sha256;
            }
            return std::nullopt;
        },
        command);
}

void appendEligible(std::vector<WorkflowActionOption>& result,
                    const packs::RuntimeCase& runtime_case, const model::WorkflowState& state,
                    model::WorkflowCommand command,
                    std::optional<std::string> record_entry_id = std::nullopt,
                    std::vector<model::FilingFieldId> required_fields = {}) {
    const auto decision =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, state, command);
    if (!decision) {
        return;
    }

    const auto digest = commandDocumentDigest(command);
    if (digest.has_value() != record_entry_id.has_value()) {
        return;
    }
    const packs::RuntimeDocketEntry* entry = nullptr;
    if (digest.has_value() && record_entry_id.has_value()) {
        entry = recordEntry(runtime_case, *record_entry_id, *digest);
        if (entry == nullptr || entry->sealed) {
            return;
        }
    }
    auto label = actionVerb(command);
    if (entry != nullptr) {
        label += QStringLiteral(": %1").arg(text(entry->title));
    }
    const auto& header = commandHeader(command);
    label += QStringLiteral(" — %1").arg(legalDateText(header.occurred_at.court_date));
    const auto description = QStringLiteral("%1 as %2; exact authored action %3 at %4")
                                 .arg(actionVerb(command), text(header.actor_id.value),
                                      actionOperationId(command), legalTimeKey(header.occurred_at));

    result.push_back(WorkflowActionOption{
        workflowActionKey(command), std::move(label), description, std::move(command),
        std::move(*decision), std::move(record_entry_id), digest, std::move(required_fields)});
}

[[nodiscard]] std::vector<std::optional<model::WorkflowDeadlineId>>
extensionDeadlineChoices(const model::WorkflowOperation& operation,
                         const model::WorkflowState& state) {
    std::vector<std::optional<model::WorkflowDeadlineId>> choices{std::nullopt};
    if (!operation.deadline_days.has_value()) {
        return choices;
    }
    for (const auto& precondition : operation.preconditions) {
        const auto* deadline = std::get_if<model::WorkflowDeadlinePrecondition>(&precondition);
        if (deadline == nullptr || deadline->condition != model::WorkflowDeadlineCondition::Open) {
            continue;
        }
        const auto open = std::ranges::find(state.deadlines, deadline->deadline_id,
                                            &model::WorkflowDeadlineRecord::deadline_id);
        if (open != state.deadlines.end() && open->status == model::WorkflowDeadlineStatus::Open) {
            choices.emplace_back(deadline->deadline_id);
        }
    }
    return choices;
}

[[nodiscard]] std::vector<model::LegalTime>
operationTimes(const model::WorkflowOperation& operation,
               const std::optional<model::LegalTime>& legacy_fallback) {
    auto times = operation.allowed_legal_times;
    if (times.empty() && legacy_fallback.has_value()) {
        times.push_back(*legacy_fallback);
    }
    std::ranges::sort(times, [](const auto& left, const auto& right) {
        if (left.instant != right.instant) {
            return left.instant < right.instant;
        }
        return std::chrono::sys_days{left.court_date.value} <
               std::chrono::sys_days{right.court_date.value};
    });
    const auto duplicates = std::ranges::unique(times);
    times.erase(duplicates.begin(), duplicates.end());
    return times;
}

} // namespace

QString workflowActionKey(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> QString {
            using Command = std::remove_cvref_t<decltype(concrete)>;
            const auto actor = text(concrete.header.actor_id.value);
            if constexpr (std::same_as<Command, model::SubmitWorkflowFiling>) {
                return QStringLiteral("filing|%1|%2|%3|%4|%5")
                    .arg(text(concrete.filing_id.value), text(concrete.filing_type.value), actor,
                         text(concrete.document_sha256), legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::EnterWorkflowOrder>) {
                return QStringLiteral("order|%1|%2|%3|%4|%5|%6|%7")
                    .arg(text(concrete.operation_id.value), actor, text(concrete.order_id.value),
                         dispositionName(concrete.disposition), text(concrete.document_sha256),
                         concrete.extension_deadline_id.has_value()
                             ? text(concrete.extension_deadline_id->value)
                             : QStringLiteral("-"),
                         legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::SetWorkflowSealed>) {
                return QStringLiteral("sealed|%1|%2|%3|%4")
                    .arg(text(concrete.operation_id.value), actor,
                         concrete.sealed ? QStringLiteral("true") : QStringLiteral("false"),
                         legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::ScheduleWorkflowArgument>) {
                return QStringLiteral("argument|%1|%2|%3|%4")
                    .arg(text(concrete.operation_id.value), actor,
                         legalDateKey(concrete.argument_date),
                         legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::IssueWorkflowJudgment>) {
                const auto disposition = std::visit(
                    [](const auto& selected) {
                        using Selection = std::remove_cvref_t<decltype(selected)>;
                        if constexpr (std::same_as<Selection, model::DispositionPlanId>) {
                            return text(selected.value);
                        } else {
                            return text(selected);
                        }
                    },
                    concrete.disposition);
                return QStringLiteral("judgment|%1|%2|%3|%4|%5")
                    .arg(text(concrete.operation_id.value), actor, text(concrete.document_sha256),
                         disposition, legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::IssueWorkflowMandate>) {
                return QStringLiteral("mandate|%1|%2|%3|%4")
                    .arg(text(concrete.operation_id.value), actor, text(concrete.document_sha256),
                         legalTimeKey(concrete.header.occurred_at));
            } else if constexpr (std::same_as<Command, model::CalculateWorkflowDeadline>) {
                return QStringLiteral("deadline|%1|%2|%3|%4")
                    .arg(text(concrete.operation_id.value), actor, text(concrete.deadline_id.value),
                         legalTimeKey(concrete.header.occurred_at));
            } else {
                return QStringLiteral("advance|%1|%2|%3")
                    .arg(text(concrete.operation_id.value), actor,
                         legalTimeKey(concrete.header.occurred_at));
            }
        },
        command);
}

auto eligibleWorkflowActions(const packs::RuntimeCase& runtime_case,
                             const model::WorkflowState& state,
                             std::optional<model::LegalTime> legacy_fallback)
    -> std::vector<WorkflowActionOption> {
    std::vector<WorkflowActionOption> result;

    for (const auto& operation : runtime_case.workflow.operations) {
        if (operation.stage_id != state.current_stage_id) {
            continue;
        }
        for (const auto& occurred_at : operationTimes(operation, legacy_fallback)) {
            for (const auto* actor : authorizedActors(runtime_case, operation.authorized_roles)) {
                const auto discriminator = QStringLiteral("%1|%2|%3")
                                               .arg(text(operation.id.value), text(actor->id.value),
                                                    legalTimeKey(occurred_at));
                const auto header = makeHeader(state, *actor, occurred_at, discriminator);
                switch (operation.opcode) {
                case model::WorkflowOpcode::EnterOrder:
                    if (operation.document_binding.has_value() &&
                        operation.document_binding->order_id.has_value() &&
                        operation.document_binding->disposition.has_value()) {
                        for (const auto& extension : extensionDeadlineChoices(operation, state)) {
                            appendEligible(
                                result, runtime_case, state,
                                model::WorkflowCommand{model::EnterWorkflowOrder{
                                    header, operation.id, *operation.document_binding->order_id,
                                    *operation.document_binding->disposition,
                                    operation.document_binding->document_sha256, extension}},
                                operation.document_binding->record_entry_id);
                        }
                    }
                    break;
                case model::WorkflowOpcode::CalculateDeadline:
                    if (operation.produced_deadline_id.has_value()) {
                        appendEligible(result, runtime_case, state,
                                       model::WorkflowCommand{model::CalculateWorkflowDeadline{
                                           header, operation.id, *operation.produced_deadline_id}});
                    }
                    break;
                case model::WorkflowOpcode::AdvanceStage:
                    if (operation.next_stage_id.has_value()) {
                        appendEligible(result, runtime_case, state,
                                       model::WorkflowCommand{
                                           model::AdvanceWorkflowStage{header, operation.id}});
                    }
                    break;
                case model::WorkflowOpcode::SetSealed:
                    appendEligible(result, runtime_case, state,
                                   model::WorkflowCommand{model::SetWorkflowSealed{
                                       header, operation.id, !state.sealed}});
                    break;
                case model::WorkflowOpcode::ScheduleArgument:
                    if (operation.expected_argument_date.has_value()) {
                        appendEligible(
                            result, runtime_case, state,
                            model::WorkflowCommand{model::ScheduleWorkflowArgument{
                                header, operation.id, *operation.expected_argument_date}});
                    }
                    break;
                case model::WorkflowOpcode::IssueJudgment:
                    if (operation.document_binding.has_value()) {
                        const auto plan =
                            operation.disposition_plan_id.has_value()
                                ? operation.disposition_plan_id
                                : runtime_case.definition.authored_disposition_plan_id;
                        if (plan.has_value()) {
                            appendEligible(result, runtime_case, state,
                                           model::WorkflowCommand{model::IssueWorkflowJudgment{
                                               header, operation.id,
                                               operation.document_binding->document_sha256, *plan}},
                                           operation.document_binding->record_entry_id);
                        }
                    }
                    break;
                case model::WorkflowOpcode::IssueMandate:
                    if (operation.document_binding.has_value()) {
                        appendEligible(
                            result, runtime_case, state,
                            model::WorkflowCommand{model::IssueWorkflowMandate{
                                header, operation.id, operation.document_binding->document_sha256}},
                            operation.document_binding->record_entry_id);
                    }
                    break;
                case model::WorkflowOpcode::AcceptFiling:
                case model::WorkflowOpcode::RejectFiling:
                case model::WorkflowOpcode::IssueDeficiency:
                    break;
                }
            }
        }
    }

    for (const auto& route : runtime_case.workflow.filing_routes) {
        if (route.stage_id != state.current_stage_id) {
            continue;
        }
        for (const auto& binding : route.filing_bindings) {
            const auto actor = std::ranges::find(runtime_case.definition.actors, binding.actor_id,
                                                 &model::CaseActor::id);
            if (actor == runtime_case.definition.actors.end()) {
                continue;
            }
            std::vector<model::WorkflowFieldValue> fields;
            fields.reserve(route.required_fields.size());
            for (const auto& field : route.required_fields) {
                fields.push_back(model::WorkflowFieldValue{field, {}});
            }
            std::vector<model::ActorId> served;
            for (const auto& candidate : runtime_case.definition.actors) {
                if (candidate.id != actor->id &&
                    std::ranges::contains(route.required_service_roles, candidate.role)) {
                    served.push_back(candidate.id);
                }
            }
            std::ranges::sort(served, {}, &model::ActorId::value);
            const auto discriminator =
                QStringLiteral("%1|%2|%3")
                    .arg(text(binding.filing_id.value), text(binding.actor_id.value),
                         legalTimeKey(binding.expected_legal_time));
            appendEligible(
                result, runtime_case, state,
                model::WorkflowCommand{model::SubmitWorkflowFiling{
                    makeHeader(state, *actor, binding.expected_legal_time, discriminator),
                    binding.filing_id, route.filing_type, binding.document_sha256,
                    std::move(fields), std::move(served), std::nullopt}},
                binding.record_entry_id, route.required_fields);
        }
    }

    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.label != right.label) {
            return left.label < right.label;
        }
        return left.key < right.key;
    });
    return result;
}

} // namespace appellate::app
