#include "appellate/storage/workflow_codec.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace model = appellate::model;
namespace storage = appellate::storage;
using storage::WorkflowCodecErrorCode;

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime legalTime() {
    constexpr std::int64_t beyond_exact_json_number = 9'007'199'254'740'993;
    return model::LegalTime{
        std::chrono::sys_seconds{std::chrono::seconds{beyond_exact_json_number}},
        date(2026, 8, 14)};
}

[[nodiscard]] model::AuthorityProvenance
provenance(model::AuthorityType type = model::AuthorityType::Rule,
           model::PrecedentialStatus status = model::PrecedentialStatus::NotApplicable) {
    return model::AuthorityProvenance{type,
                                      "us.federal",
                                      "us.ca4",
                                      status,
                                      true,
                                      "2026-08-11",
                                      "Fed. R. App. P. test",
                                      "https://www.ca4.uscourts.gov/rules/Rule03.html"};
}

[[nodiscard]] model::AuthorityRef authorityRef(std::string id, std::string proposition,
                                               bool with_provenance = false) {
    return model::AuthorityRef{model::AuthorityId{std::move(id)}, "Fed. R. App. P. test",
                               "2026-08-11", std::move(proposition),
                               with_provenance ? std::optional{provenance()} : std::nullopt};
}

[[nodiscard]] model::AuthorityBasis authority(bool with_provenance = false) {
    return model::AuthorityBasis{
        authorityRef("test.authority.primary", "Primary workflow proposition", with_provenance),
        {authorityRef("test.authority.supporting", "Supporting workflow proposition",
                      with_provenance)}};
}

[[nodiscard]] model::WorkflowCommandHeader commandHeader(std::string command_id) {
    return model::WorkflowCommandHeader{"test.session.one",
                                        model::WorkflowCommandId{std::move(command_id)},
                                        model::ActorId{"test.actor.court"}, legalTime()};
}

[[nodiscard]] model::WorkflowEventHeader
eventHeader(std::string command_id, std::string operation_id, std::uint64_t sequence = 1,
            std::uint32_t index = 0, std::uint32_t count = 1) {
    return model::WorkflowEventHeader{"test.session.one",
                                      model::WorkflowId{"test.workflow.appeal"},
                                      model::WorkflowCommandId{std::move(command_id)},
                                      model::WorkflowOperationId{std::move(operation_id)},
                                      sequence,
                                      index,
                                      count,
                                      legalTime(),
                                      authority()};
}

[[nodiscard]] model::SubmitWorkflowFiling submitCommand() {
    auto header = commandHeader("test.command.submit");
    header.actor_id = model::ActorId{"test.actor.appellant"};
    return model::SubmitWorkflowFiling{
        std::move(header),
        model::WorkflowFilingId{"test.filing.opening"},
        model::FilingTypeId{"test.filing-type.opening"},
        std::string(64, 'a'),
        {{model::FilingFieldId{"test.field.signature"}, "Jane Example"},
         {model::FilingFieldId{"test.field.optional"}, ""}},
        {model::ActorId{"test.actor.appellee"}},
        model::WorkflowDeficiencyId{"test.deficiency.prior"}};
}

[[nodiscard]] std::vector<model::WorkflowCommand> commands() {
    return {
        submitCommand(),
        model::EnterWorkflowOrder{commandHeader("test.command.order"),
                                  model::WorkflowOperationId{"test.operation.order"},
                                  model::WorkflowOrderId{"test.order.extension"},
                                  model::WorkflowOrderDisposition::Granted, std::string(64, 'b'),
                                  model::WorkflowDeadlineId{"test.deadline.response"}},
        model::SetWorkflowSealed{commandHeader("test.command.seal"),
                                 model::WorkflowOperationId{"test.operation.seal"}, true},
        model::ScheduleWorkflowArgument{commandHeader("test.command.argument"),
                                        model::WorkflowOperationId{"test.operation.argument"},
                                        date(2026, 10, 20)},
        model::IssueWorkflowJudgment{commandHeader("test.command.judgment"),
                                     model::WorkflowOperationId{"test.operation.judgment"},
                                     std::string(64, 'c'), "Affirmed in part"},
        model::IssueWorkflowMandate{commandHeader("test.command.mandate"),
                                    model::WorkflowOperationId{"test.operation.mandate"},
                                    std::string(64, 'd')},
        model::CalculateWorkflowDeadline{commandHeader("test.command.deadline"),
                                         model::WorkflowOperationId{"test.operation.deadline"},
                                         model::WorkflowDeadlineId{"test.deadline.opening"}},
        model::AdvanceWorkflowStage{commandHeader("test.command.advance"),
                                    model::WorkflowOperationId{"test.operation.advance"}},
    };
}

[[nodiscard]] std::vector<model::WorkflowEvent> events() {
    return {
        model::WorkflowFilingAccepted{eventHeader("test.command.accept", "test.operation.accept"),
                                      model::WorkflowFilingId{"test.filing.opening"},
                                      model::FilingTypeId{"test.filing-type.opening"},
                                      model::ActorId{"test.actor.appellant"},
                                      std::string(64, 'a'),
                                      {model::ActorId{"test.actor.appellee"}},
                                      model::WorkflowDeficiencyId{"test.deficiency.prior"},
                                      model::WorkflowDeadlineId{"test.deadline.prior"}},
        model::WorkflowFilingRejected{
            eventHeader("test.command.reject", "test.operation.reject", 2),
            model::WorkflowFilingId{"test.filing.late"},
            model::FilingTypeId{"test.filing-type.opening"}, model::ActorId{"test.actor.appellant"},
            model::WorkflowFilingRejectionReason::DeadlineExpired},
        model::WorkflowDeficiencyIssued{
            eventHeader("test.command.deficiency", "test.operation.deficiency", 3, 0, 2),
            model::WorkflowDeficiencyId{"test.deficiency.new"},
            model::WorkflowFilingId{"test.filing.deficient"},
            model::FilingTypeId{"test.filing-type.opening"},
            model::ActorId{"test.actor.appellant"},
            {model::WorkflowRequirementId{"test.requirement.signature"},
             model::WorkflowRequirementId{"test.requirement.service"}},
            model::WorkflowDeadlineId{"test.deadline.cure"}},
        model::WorkflowDeadlineCalculated{
            eventHeader("test.command.deficiency", "test.operation.deadline", 4, 1, 2),
            model::WorkflowDeadlineId{"test.deadline.cure"},
            model::WorkflowDeadlinePurpose::DeficiencyCure, date(2026, 8, 14), date(2026, 8, 21)},
        model::WorkflowOrderEntered{
            eventHeader("test.command.order", "test.operation.order", 5),
            model::WorkflowOrderId{"test.order.extension"},
            model::WorkflowOrderDisposition::Granted, std::string(64, 'b'),
            model::WorkflowDeadlineExtension{model::WorkflowDeadlineId{"test.deadline.response"},
                                             date(2026, 9, 1), date(2026, 9, 8)}},
        model::WorkflowStageAdvanced{
            eventHeader("test.command.advance", "test.operation.advance", 6),
            model::WorkflowStageId{"test.stage.briefing"},
            model::WorkflowStageId{"test.stage.argument"}},
        model::WorkflowSealedSet{eventHeader("test.command.seal", "test.operation.seal", 7), true},
        model::WorkflowArgumentScheduled{
            eventHeader("test.command.argument", "test.operation.argument", 8), date(2026, 10, 20),
            model::WorkflowStageId{"test.stage.argument"}},
        model::WorkflowJudgmentIssued{
            eventHeader("test.command.judgment", "test.operation.judgment", 9),
            std::string(64, 'c'), "Affirmed in part", model::WorkflowStageId{"test.stage.mandate"}},
        model::WorkflowMandateIssued{
            eventHeader("test.command.mandate", "test.operation.mandate", 10), std::string(64, 'd'),
            std::optional<model::WorkflowStageId>{}},
    };
}

[[nodiscard]] QJsonObject commandObject(const model::WorkflowCommand& command) {
    const auto encoded = storage::encodeWorkflowCommand(command);
    if (!encoded) {
        QTest::qFail(qPrintable(encoded.error().message), __FILE__, __LINE__);
        return {};
    }
    return QJsonDocument::fromJson(*encoded).object();
}

[[nodiscard]] QJsonObject eventObject(const model::WorkflowEvent& event) {
    const auto encoded = storage::encodeWorkflowEvent(event);
    if (!encoded) {
        QTest::qFail(qPrintable(encoded.error().message), __FILE__, __LINE__);
        return {};
    }
    return QJsonDocument::fromJson(*encoded).object();
}

[[nodiscard]] QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

class WorkflowCodecTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripsEveryCommandVariant();
    void roundTripsEveryEventVariant();
    void preservesLegacyEventSchemaOneBytes();
    void roundTripsCompleteProvenanceInEventSchemaTwo();
    void rejectsAuthoritySchemaDowngradesAndMixedForms();
    void rejectsMutatedProvenance();
    void roundTripsEveryEnumValue();
    void rejectsUnknownTypesVersionsAndKeys();
    void rejectsMalformedIdsDigestsTimesAndEnums();
    void rejectsIncompleteAuthorityAndInvalidCollections();
    void rejectsDuplicateSemanticMembers();
    void enforcesPayloadAndEventGroupingBounds();
};

void WorkflowCodecTest::roundTripsEveryCommandVariant() {
    const QStringList expected_types{
        QStringLiteral("filing.submit"),      QStringLiteral("order.enter"),
        QStringLiteral("sealed.set"),         QStringLiteral("argument.schedule"),
        QStringLiteral("judgment.issue"),     QStringLiteral("mandate.issue"),
        QStringLiteral("deadline.calculate"), QStringLiteral("stage.advance")};
    const auto values = commands();
    QCOMPARE(values.size(), static_cast<std::size_t>(expected_types.size()));
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto first = storage::encodeWorkflowCommand(values.at(index));
        const auto second = storage::encodeWorkflowCommand(values.at(index));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(*first, *second);
        QCOMPARE(storage::workflowCommandType(values.at(index)),
                 expected_types.at(static_cast<qsizetype>(index)));

        const auto decoded = storage::decodeWorkflowCommand(*first);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == values.at(index));
        const auto reencoded = storage::encodeWorkflowCommand(*decoded);
        QVERIFY(reencoded.has_value());
        QCOMPARE(*reencoded, *first);
    }
}

void WorkflowCodecTest::roundTripsEveryEventVariant() {
    const QStringList expected_types{QStringLiteral("filing.accepted"),
                                     QStringLiteral("filing.rejected"),
                                     QStringLiteral("filing.deficiency_issued"),
                                     QStringLiteral("deadline.calculated"),
                                     QStringLiteral("order.entered"),
                                     QStringLiteral("stage.advanced"),
                                     QStringLiteral("sealed.set"),
                                     QStringLiteral("argument.scheduled"),
                                     QStringLiteral("judgment.issued"),
                                     QStringLiteral("mandate.issued")};
    const auto values = events();
    QCOMPARE(values.size(), static_cast<std::size_t>(expected_types.size()));
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto first = storage::encodeWorkflowEvent(values.at(index));
        const auto second = storage::encodeWorkflowEvent(values.at(index));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(*first, *second);
        QCOMPARE(storage::workflowEventType(values.at(index)),
                 expected_types.at(static_cast<qsizetype>(index)));
        QCOMPARE(storage::workflowPrimaryAuthorityId(values.at(index)),
                 QStringLiteral("test.authority.primary"));

        const auto decoded = storage::decodeWorkflowEvent(*first);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == values.at(index));
        const auto reencoded = storage::encodeWorkflowEvent(*decoded);
        QVERIFY(reencoded.has_value());
        QCOMPARE(*reencoded, *first);
    }

    auto no_deadline = std::get<model::WorkflowDeficiencyIssued>(values.at(2));
    no_deadline.cure_deadline_id.reset();
    const auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{no_deadline});
    QVERIFY(encoded.has_value());
    const auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == model::WorkflowEvent{no_deadline});
    const auto payload =
        QJsonDocument::fromJson(*encoded).object().value(QStringLiteral("payload")).toObject();
    QVERIFY(payload.value(QStringLiteral("cure_deadline_id")).isNull());
}

void WorkflowCodecTest::preservesLegacyEventSchemaOneBytes() {
    const auto event = events().front();
    const auto encoded = storage::encodeWorkflowEvent(event);
    QVERIFY(encoded.has_value());
    const auto envelope = QJsonDocument::fromJson(*encoded).object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 1);
    const auto primary = envelope.value(QStringLiteral("payload"))
                             .toObject()
                             .value(QStringLiteral("authority"))
                             .toObject()
                             .value(QStringLiteral("primary"))
                             .toObject();
    QVERIFY(!primary.contains(QStringLiteral("provenance")));
    const auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    const auto reencoded = storage::encodeWorkflowEvent(*decoded);
    QVERIFY(reencoded.has_value());
    QCOMPARE(*reencoded, *encoded);
}

void WorkflowCodecTest::roundTripsCompleteProvenanceInEventSchemaTwo() {
    const std::vector types{model::AuthorityType::Constitution,
                            model::AuthorityType::Statute,
                            model::AuthorityType::Rule,
                            model::AuthorityType::Regulation,
                            model::AuthorityType::Case,
                            model::AuthorityType::Order,
                            model::AuthorityType::AdministrativeDecision,
                            model::AuthorityType::Other};
    const std::vector statuses{model::PrecedentialStatus::NotApplicable,
                               model::PrecedentialStatus::Precedential,
                               model::PrecedentialStatus::Nonprecedential};
    for (const auto type : types) {
        for (const auto status : statuses) {
            auto concrete = std::get<model::WorkflowFilingAccepted>(events().front());
            concrete.header.authority = authority(true);
            concrete.header.authority.primary.provenance = provenance(type, status);
            concrete.header.authority.supporting.front().provenance = provenance(type, status);
            const model::WorkflowEvent event = concrete;
            const auto encoded = storage::encodeWorkflowEvent(event);
            QVERIFY(encoded.has_value());
            const auto envelope = QJsonDocument::fromJson(*encoded).object();
            QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 2);
            const auto decoded = storage::decodeWorkflowEvent(*encoded);
            QVERIFY(decoded.has_value());
            QVERIFY(*decoded == event);
            const auto reencoded = storage::encodeWorkflowEvent(*decoded);
            QVERIFY(reencoded.has_value());
            QCOMPARE(*reencoded, *encoded);
        }
    }

    auto unicode_text = std::string{};
    unicode_text.reserve(2000 * std::string("한").size());
    for (int index = 0; index < 2000; ++index) {
        unicode_text += "한";
    }
    auto unicode = std::get<model::WorkflowFilingAccepted>(events().front());
    unicode.header.authority = authority(true);
    unicode.header.authority.primary.citation = unicode_text;
    unicode.header.authority.primary.proposition = unicode_text;
    unicode.header.authority.primary.provenance->locator = unicode_text;
    auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{unicode});
    QVERIFY(encoded.has_value());
    auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == model::WorkflowEvent{unicode});

    unicode.header.authority.primary.proposition = "invalid\nproposition";
    encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{unicode});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);
}

void WorkflowCodecTest::rejectsAuthoritySchemaDowngradesAndMixedForms() {
    auto concrete = std::get<model::WorkflowFilingAccepted>(events().front());
    concrete.header.authority = authority(true);
    concrete.header.authority.supporting.front().provenance.reset();
    auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{concrete});
    QVERIFY(!encoded.has_value());
    QCOMPARE(encoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    concrete.header.authority = authority(true);
    auto envelope = eventObject(model::WorkflowEvent{concrete});
    envelope.insert(QStringLiteral("schema_version"), 1);
    auto decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    envelope = eventObject(events().front());
    envelope.insert(QStringLiteral("schema_version"), 2);
    decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    envelope = eventObject(model::WorkflowEvent{concrete});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto supporting = authority_object.value(QStringLiteral("supporting")).toArray();
    auto supporting_ref = supporting.first().toObject();
    supporting_ref.remove(QStringLiteral("provenance"));
    supporting.replace(0, supporting_ref);
    authority_object.insert(QStringLiteral("supporting"), supporting);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    envelope = eventObject(model::WorkflowEvent{concrete});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    auto source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("checked_on"), QStringLiteral("2025-08-11"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);
}

void WorkflowCodecTest::rejectsMutatedProvenance() {
    auto concrete = std::get<model::WorkflowFilingAccepted>(events().front());
    concrete.header.authority = authority(true);
    auto envelope = eventObject(model::WorkflowEvent{concrete});
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    auto source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("source_url"), QStringLiteral("http://example.invalid/rule"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    auto decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    envelope = eventObject(model::WorkflowEvent{concrete});
    payload = envelope.value(QStringLiteral("payload")).toObject();
    authority_object = payload.value(QStringLiteral("authority")).toObject();
    primary = authority_object.value(QStringLiteral("primary")).toObject();
    source = primary.value(QStringLiteral("provenance")).toObject();
    source.insert(QStringLiteral("precedential_status"), QStringLiteral("binding"));
    primary.insert(QStringLiteral("provenance"), source);
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    envelope.insert(QStringLiteral("payload"), payload);
    decoded = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);
}

void WorkflowCodecTest::roundTripsEveryEnumValue() {
    for (const auto disposition :
         {model::WorkflowOrderDisposition::Granted, model::WorkflowOrderDisposition::Denied,
          model::WorkflowOrderDisposition::Other}) {
        auto command = std::get<model::EnterWorkflowOrder>(commands().at(1));
        command.disposition = disposition;
        const auto encoded = storage::encodeWorkflowCommand(model::WorkflowCommand{command});
        QVERIFY(encoded.has_value());
        const auto decoded = storage::decodeWorkflowCommand(*encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(std::get<model::EnterWorkflowOrder>(*decoded).disposition, disposition);
    }

    for (const auto reason : {model::WorkflowFilingRejectionReason::UnauthorizedActor,
                              model::WorkflowFilingRejectionReason::IneligibleFiling,
                              model::WorkflowFilingRejectionReason::NonconformingFiling,
                              model::WorkflowFilingRejectionReason::DeadlineExpired,
                              model::WorkflowFilingRejectionReason::UnknownDeficiency}) {
        auto event = std::get<model::WorkflowFilingRejected>(events().at(1));
        event.reason = reason;
        const auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
        QVERIFY(encoded.has_value());
        const auto decoded = storage::decodeWorkflowEvent(*encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(std::get<model::WorkflowFilingRejected>(*decoded).reason, reason);
    }

    for (const auto purpose :
         {model::WorkflowDeadlinePurpose::Filing, model::WorkflowDeadlinePurpose::DeficiencyCure}) {
        auto event = std::get<model::WorkflowDeadlineCalculated>(events().at(3));
        event.purpose = purpose;
        const auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
        QVERIFY(encoded.has_value());
        const auto decoded = storage::decodeWorkflowEvent(*encoded);
        QVERIFY(decoded.has_value());
        QCOMPARE(std::get<model::WorkflowDeadlineCalculated>(*decoded).purpose, purpose);
    }
}

void WorkflowCodecTest::rejectsUnknownTypesVersionsAndKeys() {
    auto envelope = commandObject(commands().front());
    envelope.insert(QStringLiteral("command_type"), QStringLiteral("filing.future"));
    auto command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::UnknownCommandType);

    envelope = commandObject(commands().front());
    envelope.insert(QStringLiteral("schema_version"), 2);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::UnsupportedVersion);

    auto event_envelope = eventObject(events().front());
    event_envelope.insert(QStringLiteral("event_type"), QStringLiteral("filing.future"));
    auto event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::UnknownEventType);

    envelope = commandObject(commands().front());
    envelope.insert(QStringLiteral("extra"), true);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::UnexpectedField);

    envelope = commandObject(commands().front());
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.remove(QStringLiteral("actor_id"));
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::MissingField);

    envelope = commandObject(commands().back());
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("next_stage_id"), QStringLiteral("test.stage.forged"));
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::UnexpectedField);

    envelope = commandObject(commands().back());
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.remove(QStringLiteral("operation_id"));
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::MissingField);

    event_envelope = eventObject(events().front());
    payload = event_envelope.value(QStringLiteral("payload")).toObject();
    auto occurred = payload.value(QStringLiteral("occurred_at")).toObject();
    occurred.insert(QStringLiteral("timezone"), QStringLiteral("UTC"));
    payload.insert(QStringLiteral("occurred_at"), occurred);
    event_envelope.insert(QStringLiteral("payload"), payload);
    event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::UnexpectedField);
}

void WorkflowCodecTest::rejectsMalformedIdsDigestsTimesAndEnums() {
    auto envelope = commandObject(commands().front());
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("command_id"), QStringLiteral("Invalid_Command"));
    envelope.insert(QStringLiteral("payload"), payload);
    auto command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidField);

    envelope = commandObject(commands().front());
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("document_sha256"), QString(64, u'A'));
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidField);

    auto invalid_order = std::get<model::EnterWorkflowOrder>(commands().at(1));
    invalid_order.disposition = static_cast<model::WorkflowOrderDisposition>(99);
    const auto invalid_order_encoding =
        storage::encodeWorkflowCommand(model::WorkflowCommand{invalid_order});
    QVERIFY(!invalid_order_encoding.has_value());
    QCOMPARE(invalid_order_encoding.error().code, WorkflowCodecErrorCode::InvalidField);

    envelope = commandObject(commands().at(3));
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("argument_date"), QStringLiteral("2026-02-30"));
    auto occurred = payload.value(QStringLiteral("occurred_at")).toObject();
    occurred.insert(QStringLiteral("instant_unix_seconds"), QStringLiteral("01"));
    payload.insert(QStringLiteral("occurred_at"), occurred);
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidField);

    envelope = commandObject(commands().at(1));
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("disposition"), QStringLiteral("reserved"));
    envelope.insert(QStringLiteral("payload"), payload);
    command = storage::decodeWorkflowCommand(compact(envelope));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidField);

    auto event_envelope = eventObject(events().at(1));
    payload = event_envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("reason"), QStringLiteral("future_reason"));
    event_envelope.insert(QStringLiteral("payload"), payload);
    const auto event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::InvalidField);

    event_envelope = eventObject(events().at(3));
    payload = event_envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("purpose"), QStringLiteral("future_purpose"));
    event_envelope.insert(QStringLiteral("payload"), payload);
    const auto deadline_event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!deadline_event.has_value());
    QCOMPARE(deadline_event.error().code, WorkflowCodecErrorCode::InvalidField);

    auto invalid_deadline = std::get<model::WorkflowDeadlineCalculated>(events().at(3));
    invalid_deadline.purpose = static_cast<model::WorkflowDeadlinePurpose>(99);
    const auto invalid_deadline_encoding =
        storage::encodeWorkflowEvent(model::WorkflowEvent{invalid_deadline});
    QVERIFY(!invalid_deadline_encoding.has_value());
    QCOMPARE(invalid_deadline_encoding.error().code, WorkflowCodecErrorCode::InvalidField);
}

void WorkflowCodecTest::rejectsIncompleteAuthorityAndInvalidCollections() {
    auto value = events().front();
    auto& accepted = std::get<model::WorkflowFilingAccepted>(value);
    accepted.header.authority.primary.source_version = "2026-02-30";
    auto encoded_event = storage::encodeWorkflowEvent(value);
    QVERIFY(!encoded_event.has_value());
    QCOMPARE(encoded_event.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    auto event_envelope = eventObject(events().front());
    auto payload = event_envelope.value(QStringLiteral("payload")).toObject();
    auto authority_object = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority_object.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("proposition"), QString{});
    authority_object.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority_object);
    event_envelope.insert(QStringLiteral("payload"), payload);
    auto event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    auto submit = submitCommand();
    submit.served_actors.push_back(submit.served_actors.front());
    auto encoded_command = storage::encodeWorkflowCommand(model::WorkflowCommand{submit});
    QVERIFY(!encoded_command.has_value());
    QCOMPARE(encoded_command.error().code, WorkflowCodecErrorCode::InvalidField);

    submit = submitCommand();
    for (std::size_t index = submit.fields.size(); index <= 256; ++index) {
        submit.fields.push_back(model::WorkflowFieldValue{
            model::FilingFieldId{"test.field.extra-" + std::to_string(index)}, "value"});
    }
    encoded_command = storage::encodeWorkflowCommand(model::WorkflowCommand{submit});
    QVERIFY(!encoded_command.has_value());
    QCOMPARE(encoded_command.error().code, WorkflowCodecErrorCode::OutOfRange);

    auto deficiency = std::get<model::WorkflowDeficiencyIssued>(events().at(2));
    deficiency.missing_requirements.clear();
    encoded_event = storage::encodeWorkflowEvent(model::WorkflowEvent{deficiency});
    QVERIFY(!encoded_event.has_value());
    QCOMPARE(encoded_event.error().code, WorkflowCodecErrorCode::OutOfRange);
}

void WorkflowCodecTest::rejectsDuplicateSemanticMembers() {
    const QByteArray duplicate_top =
        R"({"command_type":"filing.submit","\u0063ommand_type":"filing.submit","payload":{},"schema_version":1})";
    auto command = storage::decodeWorkflowCommand(duplicate_top);
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::DuplicateMember);

    auto encoded = storage::encodeWorkflowCommand(commands().front());
    QVERIFY(encoded.has_value());
    const auto replaced = encoded->replace(
        QByteArrayLiteral("\"actor_id\":"),
        QByteArrayLiteral("\"\\u0061ctor_id\":\"test.actor.other\",\"actor_id\":"));
    command = storage::decodeWorkflowCommand(replaced);
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::DuplicateMember);
}

void WorkflowCodecTest::enforcesPayloadAndEventGroupingBounds() {
    const QByteArray oversized(1024 * 1024 + 1, 'x');
    auto command = storage::decodeWorkflowCommand(oversized);
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::PayloadTooLarge);

    auto too_large_to_encode = submitCommand();
    too_large_to_encode.fields.clear();
    for (std::size_t index = 0; index < 256; ++index) {
        too_large_to_encode.fields.push_back(model::WorkflowFieldValue{
            model::FilingFieldId{"test.field.large-" + std::to_string(index)},
            std::string(4096, 'x')});
    }
    const auto oversized_encoding =
        storage::encodeWorkflowCommand(model::WorkflowCommand{too_large_to_encode});
    QVERIFY(!oversized_encoding.has_value());
    QCOMPARE(oversized_encoding.error().code, WorkflowCodecErrorCode::PayloadTooLarge);

    command = storage::decodeWorkflowCommand(QByteArrayLiteral("{not-json}"));
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidJson);

    const QByteArray invalid_utf8{"{\"x\":\"\xFF\"}", 9};
    command = storage::decodeWorkflowCommand(invalid_utf8);
    QVERIFY(!command.has_value());
    QCOMPARE(command.error().code, WorkflowCodecErrorCode::InvalidJson);

    auto envelope = eventObject(events().front());
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("command_event_count"), QStringLiteral("4"));
    envelope.insert(QStringLiteral("payload"), payload);
    auto event = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::OutOfRange);

    envelope = eventObject(events().front());
    payload = envelope.value(QStringLiteral("payload")).toObject();
    payload.insert(QStringLiteral("sequence"),
                   QString::number(std::numeric_limits<qulonglong>::max()));
    envelope.insert(QStringLiteral("payload"), payload);
    event = storage::decodeWorkflowEvent(compact(envelope));
    QVERIFY(!event.has_value());
    QCOMPARE(event.error().code, WorkflowCodecErrorCode::OutOfRange);

    const auto encoded = storage::encodeWorkflowEvent(events().front());
    QVERIFY(encoded.has_value());
    QVERIFY(encoded->contains(QByteArrayLiteral("\"9007199254740993\"")));
}

} // namespace

QTEST_GUILESS_MAIN(WorkflowCodecTest)

#include "tst_workflow_codec.moc"
