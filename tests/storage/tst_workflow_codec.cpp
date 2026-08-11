#include "appellate/storage/workflow_codec.hpp"

#include <QCryptographicHash>
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

[[nodiscard]] model::DispositionPlan dispositionPlan() {
    return model::DispositionPlan{
        model::DispositionPlanId{"test.disposition.partial"},
        model::DispositionFinality::Final,
        std::string(64, 'e'),
        {model::DispositionComponent{model::CaseIssueId{"test.issue.finality"},
                                     model::DispositionTargetId{"test.target.claim-one"},
                                     model::DispositionScope::Whole,
                                     model::DispositionAction::Dismiss,
                                     true,
                                     {model::AuthorityId{"test.authority.primary"}},
                                     {model::RecordAnchorId{"test.anchor.order"}}},
         model::DispositionComponent{model::CaseIssueId{"test.issue.merits"},
                                     model::DispositionTargetId{"test.target.claim-two"},
                                     model::DispositionScope::Part,
                                     model::DispositionAction::Deny,
                                     false,
                                     {model::AuthorityId{"test.authority.supporting"}},
                                     {model::RecordAnchorId{"test.anchor.brief"}}}}};
}

[[nodiscard]] std::vector<model::WorkflowPrecondition> preconditions() {
    return {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing-type.opening"}, true},
        model::WorkflowOrderPrecondition{model::WorkflowOrderId{"test.order.submission"},
                                         model::WorkflowOrderDisposition::Granted},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.response"},
                                            model::WorkflowDeadlineCondition::Open},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.response"},
                                            model::WorkflowDeadlineCondition::Elapsed},
        model::WorkflowArgumentPrecondition{false},
        model::WorkflowJudgmentPrecondition{false},
    };
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
    void roundTripsStructuredDispositionSchemaThree();
    void roundTripsMaximumStructuredPayload();
    void roundTripsPreconditionSnapshotsSchemaThree();
    void roundTripsAndFencesExtendedSchemaFour();
    void roundTripsAndFencesInstanceSchemaFive();
    void rejectsStructuredVersionConfusionAndTampering();
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
    const QByteArray frozen_command{
        R"json({"command_type":"judgment.issue","payload":{"actor_id":"test.actor.court","command_id":"test.command.judgment","disposition":"Affirmed in part","document_sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","occurred_at":{"court_date":"2026-08-14","instant_unix_seconds":"9007199254740993"},"operation_id":"test.operation.judgment","session_id":"test.session.one"},"schema_version":1})json"};
    const auto decoded_command = storage::decodeWorkflowCommand(frozen_command);
    QVERIFY(decoded_command.has_value());
    QVERIFY(*decoded_command == commands().at(4));
    const auto reencoded_command = storage::encodeWorkflowCommand(*decoded_command);
    QVERIFY(reencoded_command.has_value());
    QCOMPARE(*reencoded_command, frozen_command);

    const QByteArray frozen_judgment_event{
        R"json({"event_type":"judgment.issued","payload":{"authority":{"primary":{"citation":"Fed. R. App. P. test","id":"test.authority.primary","proposition":"Primary workflow proposition","source_version":"2026-08-11"},"supporting":[{"citation":"Fed. R. App. P. test","id":"test.authority.supporting","proposition":"Supporting workflow proposition","source_version":"2026-08-11"}]},"command_event_count":"1","command_event_index":"0","command_id":"test.command.judgment","disposition":"Affirmed in part","document_sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","next_stage_id":"test.stage.mandate","occurred_at":{"court_date":"2026-08-14","instant_unix_seconds":"9007199254740993"},"operation_id":"test.operation.judgment","sequence":"9","session_id":"test.session.one","workflow_id":"test.workflow.appeal"},"schema_version":1})json"};
    const auto decoded_judgment_event = storage::decodeWorkflowEvent(frozen_judgment_event);
    QVERIFY(decoded_judgment_event.has_value());
    QVERIFY(*decoded_judgment_event == events().at(8));
    const auto reencoded_judgment_event = storage::encodeWorkflowEvent(*decoded_judgment_event);
    QVERIFY(reencoded_judgment_event.has_value());
    QCOMPARE(*reencoded_judgment_event, frozen_judgment_event);

    const QByteArray frozen_schema_two_judgment_event{
        R"json({"event_type":"judgment.issued","payload":{"authority":{"primary":{"citation":"Fed. R. App. P. test","id":"test.authority.primary","proposition":"Primary workflow proposition","provenance":{"authority_type":"rule","checked_on":"2026-08-11","issuing_body_id":"us.ca4","jurisdiction_id":"us.federal","locator":"Fed. R. App. P. test","official_source":true,"precedential_status":"not_applicable","source_url":"https://www.ca4.uscourts.gov/rules/Rule03.html"},"source_version":"2026-08-11"},"supporting":[{"citation":"Fed. R. App. P. test","id":"test.authority.supporting","proposition":"Supporting workflow proposition","provenance":{"authority_type":"rule","checked_on":"2026-08-11","issuing_body_id":"us.ca4","jurisdiction_id":"us.federal","locator":"Fed. R. App. P. test","official_source":true,"precedential_status":"not_applicable","source_url":"https://www.ca4.uscourts.gov/rules/Rule03.html"},"source_version":"2026-08-11"}]},"command_event_count":"1","command_event_index":"0","command_id":"test.command.judgment","disposition":"Affirmed in part","document_sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","next_stage_id":"test.stage.mandate","occurred_at":{"court_date":"2026-08-14","instant_unix_seconds":"9007199254740993"},"operation_id":"test.operation.judgment","sequence":"9","session_id":"test.session.one","workflow_id":"test.workflow.appeal"},"schema_version":2})json"};
    const auto decoded_schema_two = storage::decodeWorkflowEvent(frozen_schema_two_judgment_event);
    QVERIFY(decoded_schema_two.has_value());
    auto expected_schema_two = std::get<model::WorkflowJudgmentIssued>(events().at(8));
    expected_schema_two.header.authority = authority(true);
    QVERIFY(*decoded_schema_two == model::WorkflowEvent{expected_schema_two});
    const auto reencoded_schema_two = storage::encodeWorkflowEvent(*decoded_schema_two);
    QVERIFY(reencoded_schema_two.has_value());
    QCOMPARE(*reencoded_schema_two, frozen_schema_two_judgment_event);

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

void WorkflowCodecTest::roundTripsStructuredDispositionSchemaThree() {
    const model::WorkflowCommand command = model::IssueWorkflowJudgment{
        commandHeader("test.command.structured-judgment"),
        model::WorkflowOperationId{"test.operation.judgment"}, std::string(64, 'f'),
        model::DispositionPlanId{"test.disposition.partial"}};
    const auto encoded_command = storage::encodeWorkflowCommand(command);
    QVERIFY(encoded_command.has_value());
    const auto command_envelope = QJsonDocument::fromJson(*encoded_command).object();
    QCOMPARE(command_envelope.value(QStringLiteral("schema_version")).toInt(), 3);
    QVERIFY(command_envelope.value(QStringLiteral("payload"))
                .toObject()
                .value(QStringLiteral("disposition"))
                .isObject());
    const auto decoded_command = storage::decodeWorkflowCommand(*encoded_command);
    QVERIFY(decoded_command.has_value());
    QVERIFY(*decoded_command == command);
    const auto reencoded_command = storage::encodeWorkflowCommand(*decoded_command);
    QVERIFY(reencoded_command.has_value());
    QCOMPARE(*reencoded_command, *encoded_command);

    auto header = eventHeader("test.command.structured-judgment", "test.operation.judgment", 12);
    header.authority = authority(true);
    const model::WorkflowEvent event =
        model::WorkflowJudgmentIssued{std::move(header), std::string(64, 'f'), dispositionPlan(),
                                      model::WorkflowStageId{"test.stage.mandate"}};
    const auto encoded_event = storage::encodeWorkflowEvent(event);
    QVERIFY(encoded_event.has_value());
    const auto event_envelope = QJsonDocument::fromJson(*encoded_event).object();
    QCOMPARE(event_envelope.value(QStringLiteral("schema_version")).toInt(), 3);
    const auto payload = event_envelope.value(QStringLiteral("payload")).toObject();
    QVERIFY(payload.value(QStringLiteral("disposition")).isObject());
    QVERIFY(payload.value(QStringLiteral("preconditions")).isArray());
    QVERIFY(payload.value(QStringLiteral("preconditions")).toArray().isEmpty());
    const auto decoded_event = storage::decodeWorkflowEvent(*encoded_event);
    QVERIFY(decoded_event.has_value());
    QVERIFY(*decoded_event == event);
    const auto reencoded_event = storage::encodeWorkflowEvent(*decoded_event);
    QVERIFY(reencoded_event.has_value());
    QCOMPARE(*reencoded_event, *encoded_event);

    auto schema_two = std::get<model::WorkflowJudgmentIssued>(events().at(8));
    schema_two.header.authority = authority(true);
    const auto encoded_schema_two = storage::encodeWorkflowEvent(model::WorkflowEvent{schema_two});
    QVERIFY(encoded_schema_two.has_value());
    const auto schema_two_envelope = QJsonDocument::fromJson(*encoded_schema_two).object();
    QCOMPARE(schema_two_envelope.value(QStringLiteral("schema_version")).toInt(), 2);
    QVERIFY(!schema_two_envelope.value(QStringLiteral("payload"))
                 .toObject()
                 .contains(QStringLiteral("preconditions")));
    const auto decoded_schema_two = storage::decodeWorkflowEvent(*encoded_schema_two);
    QVERIFY(decoded_schema_two.has_value());
    const auto reencoded_schema_two = storage::encodeWorkflowEvent(*decoded_schema_two);
    QVERIFY(reencoded_schema_two.has_value());
    QCOMPARE(*reencoded_schema_two, *encoded_schema_two);
}

void WorkflowCodecTest::roundTripsMaximumStructuredPayload() {
    const auto maximumId = [](std::string prefix, int component, int reference) {
        const auto suffix = std::to_string(component) + "-" + std::to_string(reference);
        return prefix + std::string(160 - prefix.size() - suffix.size(), 'x') + suffix;
    };
    auto plan = dispositionPlan();
    plan.components.clear();
    for (int component_index = 0; component_index < 32; ++component_index) {
        std::vector<model::AuthorityId> authorities;
        std::vector<model::RecordAnchorId> anchors;
        authorities.reserve(32);
        anchors.reserve(32);
        for (int reference_index = 0; reference_index < 32; ++reference_index) {
            authorities.push_back(
                model::AuthorityId{maximumId("test.authority.", component_index, reference_index)});
            anchors.push_back(
                model::RecordAnchorId{maximumId("test.anchor.", component_index, reference_index)});
        }
        plan.components.push_back(model::DispositionComponent{
            model::CaseIssueId{maximumId("test.issue.", component_index, 0)},
            model::DispositionTargetId{maximumId("test.target.", component_index, 0)},
            model::DispositionScope::Part, model::DispositionAction::Vacate, true,
            std::move(authorities), std::move(anchors)});
    }
    auto header = eventHeader("test.command.maximum-judgment", "test.operation.judgment", 14);
    header.authority = authority(true);
    const model::WorkflowEvent event = model::WorkflowJudgmentIssued{
        std::move(header), std::string(64, 'f'), std::move(plan), std::nullopt};
    const auto encoded = storage::encodeWorkflowEvent(event);
    QVERIFY(encoded.has_value());
    QVERIFY(encoded->size() < 1024 * 1024);
    const auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == event);
}

void WorkflowCodecTest::roundTripsPreconditionSnapshotsSchemaThree() {
    auto event = std::get<model::WorkflowSealedSet>(events().at(6));
    event.header.authority = authority(true);
    event.header.preconditions = preconditions();
    const auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
    QVERIFY(encoded.has_value());
    QCOMPARE(QCryptographicHash::hash(*encoded, QCryptographicHash::Sha256).toHex(),
             QByteArray("4fc6c425be07f6b8c998889fe2dc7394c8192eec2df64a8faca3c66908b0e337"));
    const auto envelope = QJsonDocument::fromJson(*encoded).object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 3);
    const auto encoded_preconditions = envelope.value(QStringLiteral("payload"))
                                           .toObject()
                                           .value(QStringLiteral("preconditions"))
                                           .toArray();
    QCOMPARE(encoded_preconditions.size(), 6);
    QCOMPARE(encoded_preconditions.at(2).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("open"));
    QCOMPARE(encoded_preconditions.at(3).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("elapsed"));
    const auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == model::WorkflowEvent{event});
    const auto reencoded = storage::encodeWorkflowEvent(*decoded);
    QVERIFY(reencoded.has_value());
    QCOMPARE(*reencoded, *encoded);

    event.header.preconditions.at(3) =
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.response"},
                                            model::WorkflowDeadlineCondition::NotElapsed};
    const auto not_elapsed = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
    QVERIFY(not_elapsed.has_value());
    const auto not_elapsed_decoded = storage::decodeWorkflowEvent(*not_elapsed);
    QVERIFY(not_elapsed_decoded.has_value());
    QVERIFY(*not_elapsed_decoded == model::WorkflowEvent{event});

    event.header.preconditions.at(3) =
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.response"},
                                            model::WorkflowDeadlineCondition::Reached};
    const auto reached = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
    QVERIFY(reached.has_value());
    const auto reached_envelope = QJsonDocument::fromJson(*reached).object();
    const auto reached_preconditions = reached_envelope.value(QStringLiteral("payload"))
                                           .toObject()
                                           .value(QStringLiteral("preconditions"))
                                           .toArray();
    QCOMPARE(reached_preconditions.at(3).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("reached"));
    const auto reached_decoded = storage::decodeWorkflowEvent(*reached);
    QVERIFY(reached_decoded.has_value());
    QVERIFY(*reached_decoded == model::WorkflowEvent{event});
}

void WorkflowCodecTest::roundTripsAndFencesExtendedSchemaFour() {
    auto named = std::get<model::WorkflowDeadlineCalculated>(events().at(3));
    named.header.authority = authority(true);
    named.produced_deadline_id = named.deadline_id;
    const auto named_encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{named});
    QVERIFY(named_encoded.has_value());
    QCOMPARE(QCryptographicHash::hash(*named_encoded, QCryptographicHash::Sha256).toHex(),
             QByteArray("798f8f236987af34d998b5fcac5ef27866e10832b24a11fbdabcb2400cddf18f"));
    auto named_envelope = QJsonDocument::fromJson(*named_encoded).object();
    QCOMPARE(named_envelope.value(QStringLiteral("schema_version")).toInt(), 4);
    auto named_payload = named_envelope.value(QStringLiteral("payload")).toObject();
    QCOMPARE(named_payload.value(QStringLiteral("produced_deadline_id")).toString(),
             QString::fromUtf8(named.deadline_id.value));
    QVERIFY(named_payload.value(QStringLiteral("deadline_base_id")).isNull());
    QVERIFY(named_payload.value(QStringLiteral("deadline_event_base")).isNull());
    QVERIFY(named_payload.value(QStringLiteral("preconditions")).toArray().isEmpty());
    const auto named_decoded = storage::decodeWorkflowEvent(*named_encoded);
    QVERIFY(named_decoded.has_value());
    QVERIFY(*named_decoded == model::WorkflowEvent{named});
    const auto named_reencoded = storage::encodeWorkflowEvent(*named_decoded);
    QVERIFY(named_reencoded.has_value());
    QCOMPARE(*named_reencoded, *named_encoded);

    auto dependent = named;
    dependent.deadline_base_id = model::WorkflowDeadlineId{"test.deadline.prior"};
    const auto dependent_encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{dependent});
    QVERIFY(dependent_encoded.has_value());
    const auto dependent_payload = QJsonDocument::fromJson(*dependent_encoded)
                                       .object()
                                       .value(QStringLiteral("payload"))
                                       .toObject();
    QCOMPARE(dependent_payload.value(QStringLiteral("deadline_base_id")).toString(),
             QStringLiteral("test.deadline.prior"));
    const auto dependent_decoded = storage::decodeWorkflowEvent(*dependent_encoded);
    QVERIFY(dependent_decoded.has_value());
    QVERIFY(*dependent_decoded == model::WorkflowEvent{dependent});

    auto judgment_based = named;
    judgment_based.deadline_event_base = model::WorkflowJudgmentOccurredDeadlineBase{};
    const auto judgment_encoded =
        storage::encodeWorkflowEvent(model::WorkflowEvent{judgment_based});
    QVERIFY(judgment_encoded.has_value());
    const auto judgment_base = QJsonDocument::fromJson(*judgment_encoded)
                                   .object()
                                   .value(QStringLiteral("payload"))
                                   .toObject()
                                   .value(QStringLiteral("deadline_event_base"))
                                   .toObject();
    QCOMPARE(judgment_base.value(QStringLiteral("kind")).toString(),
             QStringLiteral("judgment_occurred"));
    const auto judgment_decoded = storage::decodeWorkflowEvent(*judgment_encoded);
    QVERIFY(judgment_decoded.has_value());
    QVERIFY(*judgment_decoded == model::WorkflowEvent{judgment_based});

    auto order_based = named;
    order_based.deadline_event_base = model::WorkflowOrderOccurredDeadlineBase{
        model::WorkflowOrderId{"test.order.rehearing"},
        model::WorkflowOperationId{"test.operation.enter-rehearing-order"}};
    const auto order_encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{order_based});
    QVERIFY(order_encoded.has_value());
    const auto order_decoded = storage::decodeWorkflowEvent(*order_encoded);
    QVERIFY(order_decoded.has_value());
    QVERIFY(*order_decoded == model::WorkflowEvent{order_based});

    auto extended = std::get<model::WorkflowSealedSet>(events().at(6));
    extended.header.authority = authority(true);
    extended.header.preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.response"},
                                            model::WorkflowDeadlineCondition::Reached},
        model::WorkflowArgumentPrecondition{true},
        model::WorkflowArgumentDatePrecondition{model::WorkflowArgumentDateCondition::Reached},
    };
    const auto extended_encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{extended});
    QVERIFY(extended_encoded.has_value());
    auto extended_envelope = QJsonDocument::fromJson(*extended_encoded).object();
    QCOMPARE(extended_envelope.value(QStringLiteral("schema_version")).toInt(), 4);
    auto extended_payload = extended_envelope.value(QStringLiteral("payload")).toObject();
    QVERIFY(!extended_payload.contains(QStringLiteral("deadline_base_id")));
    QVERIFY(!extended_payload.contains(QStringLiteral("deadline_event_base")));
    QVERIFY(!extended_payload.contains(QStringLiteral("produced_deadline_id")));
    const auto extended_decoded = storage::decodeWorkflowEvent(*extended_encoded);
    QVERIFY(extended_decoded.has_value());
    QVERIFY(*extended_decoded == model::WorkflowEvent{extended});

    auto downgraded = extended_envelope;
    downgraded.insert(QStringLiteral("schema_version"), 3);
    auto rejected = storage::decodeWorkflowEvent(compact(downgraded));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::UnsupportedVersion);

    auto relabeled =
        eventObject(model::WorkflowEvent{std::get<model::WorkflowSealedSet>(events().at(6))});
    relabeled.insert(QStringLiteral("schema_version"), 4);
    auto relabeled_payload = relabeled.value(QStringLiteral("payload")).toObject();
    relabeled_payload.insert(QStringLiteral("preconditions"), QJsonArray{});
    relabeled.insert(QStringLiteral("payload"), relabeled_payload);
    rejected = storage::decodeWorkflowEvent(compact(relabeled));
    QVERIFY(!rejected.has_value());

    auto deadline_downgrade = named_envelope;
    deadline_downgrade.insert(QStringLiteral("schema_version"), 3);
    rejected = storage::decodeWorkflowEvent(compact(deadline_downgrade));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::UnexpectedField);

    auto missing_output = named_envelope;
    auto tampered_payload = missing_output.value(QStringLiteral("payload")).toObject();
    tampered_payload.remove(QStringLiteral("produced_deadline_id"));
    missing_output.insert(QStringLiteral("payload"), tampered_payload);
    rejected = storage::decodeWorkflowEvent(compact(missing_output));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::MissingField);

    auto mismatched_output = named_envelope;
    tampered_payload = mismatched_output.value(QStringLiteral("payload")).toObject();
    tampered_payload.insert(QStringLiteral("produced_deadline_id"),
                            QStringLiteral("test.deadline.substituted"));
    mismatched_output.insert(QStringLiteral("payload"), tampered_payload);
    rejected = storage::decodeWorkflowEvent(compact(mismatched_output));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::InvalidField);

    auto dual_base = QJsonDocument::fromJson(*dependent_encoded).object();
    tampered_payload = dual_base.value(QStringLiteral("payload")).toObject();
    tampered_payload.insert(
        QStringLiteral("deadline_event_base"),
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("judgment_occurred")}});
    dual_base.insert(QStringLiteral("payload"), tampered_payload);
    rejected = storage::decodeWorkflowEvent(compact(dual_base));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::InvalidField);

    auto unnamed_reached = named;
    unnamed_reached.produced_deadline_id.reset();
    unnamed_reached.header.preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.prior"},
                                            model::WorkflowDeadlineCondition::Reached}};
    const auto unnamed_encoding =
        storage::encodeWorkflowEvent(model::WorkflowEvent{unnamed_reached});
    QVERIFY(!unnamed_encoding.has_value());
    QCOMPARE(unnamed_encoding.error().code, WorkflowCodecErrorCode::InvalidField);
}

void WorkflowCodecTest::roundTripsAndFencesInstanceSchemaFive() {
    auto event = std::get<model::WorkflowDeadlineCalculated>(events().at(3));
    event.header.authority = authority(true);
    event.produced_deadline_id = event.deadline_id;
    event.header.preconditions = {
        model::WorkflowFilingInstancePrecondition{
            model::FilingTypeId{"test.filing-type.opening"}, true,
            model::ActorId{"test.actor.appellant"}, model::WorkflowFilingId{"test.filing.opening"},
            model::WorkflowOperationId{"test.operation.accept"}, "test.record.entry-opening",
            std::string(64, 'a')},
        model::WorkflowOrderInstancePrecondition{model::WorkflowOrderId{"test.order.extension"},
                                                 model::WorkflowOrderDisposition::Granted,
                                                 model::WorkflowOperationId{"test.operation.order"},
                                                 "test.record.entry-order", std::string(64, 'b')},
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.prior"},
                                            model::WorkflowDeadlineCondition::Reached},
    };

    const auto encoded = storage::encodeWorkflowEvent(model::WorkflowEvent{event});
    QVERIFY(encoded.has_value());
    auto envelope = QJsonDocument::fromJson(*encoded).object();
    QCOMPARE(envelope.value(QStringLiteral("schema_version")).toInt(), 5);
    const auto payload = envelope.value(QStringLiteral("payload")).toObject();
    QCOMPARE(payload.value(QStringLiteral("produced_deadline_id")).toString(),
             QString::fromUtf8(event.deadline_id.value));
    QCOMPARE(payload.value(QStringLiteral("preconditions")).toArray().size(), 3);
    const auto decoded = storage::decodeWorkflowEvent(*encoded);
    QVERIFY(decoded.has_value());
    QVERIFY(*decoded == model::WorkflowEvent{event});
    const auto reencoded = storage::encodeWorkflowEvent(*decoded);
    QVERIFY(reencoded.has_value());
    QCOMPARE(*reencoded, *encoded);

    auto instance_only = std::get<model::WorkflowSealedSet>(events().at(6));
    instance_only.header.authority = authority(true);
    instance_only.header.preconditions = {event.header.preconditions.front()};
    const auto instance_only_encoded =
        storage::encodeWorkflowEvent(model::WorkflowEvent{instance_only});
    QVERIFY(instance_only_encoded.has_value());
    const auto instance_only_envelope = QJsonDocument::fromJson(*instance_only_encoded).object();
    QCOMPARE(instance_only_envelope.value(QStringLiteral("schema_version")).toInt(), 5);
    const auto instance_only_payload =
        instance_only_envelope.value(QStringLiteral("payload")).toObject();
    QVERIFY(!instance_only_payload.contains(QStringLiteral("deadline_base_id")));
    QVERIFY(!instance_only_payload.contains(QStringLiteral("deadline_event_base")));
    QVERIFY(!instance_only_payload.contains(QStringLiteral("produced_deadline_id")));
    const auto instance_only_decoded = storage::decodeWorkflowEvent(*instance_only_encoded);
    QVERIFY(instance_only_decoded.has_value());
    QVERIFY(*instance_only_decoded == model::WorkflowEvent{instance_only});
    for (const auto older_version : {1, 2, 3, 4}) {
        auto older = instance_only_envelope;
        older.insert(QStringLiteral("schema_version"), older_version);
        const auto older_result = storage::decodeWorkflowEvent(compact(older));
        QVERIFY(!older_result.has_value());
    }

    auto downgraded = envelope;
    downgraded.insert(QStringLiteral("schema_version"), 4);
    auto rejected = storage::decodeWorkflowEvent(compact(downgraded));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::UnsupportedVersion);

    auto version_four = std::get<model::WorkflowSealedSet>(events().at(6));
    version_four.header.authority = authority(true);
    version_four.header.preconditions = {
        model::WorkflowDeadlinePrecondition{model::WorkflowDeadlineId{"test.deadline.prior"},
                                            model::WorkflowDeadlineCondition::Reached}};
    const auto version_four_encoded =
        storage::encodeWorkflowEvent(model::WorkflowEvent{version_four});
    QVERIFY(version_four_encoded.has_value());
    auto relabeled = QJsonDocument::fromJson(*version_four_encoded).object();
    QCOMPARE(relabeled.value(QStringLiteral("schema_version")).toInt(), 4);
    relabeled.insert(QStringLiteral("schema_version"), 5);
    rejected = storage::decodeWorkflowEvent(compact(relabeled));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    auto tampered = envelope;
    auto tampered_payload = tampered.value(QStringLiteral("payload")).toObject();
    auto guards = tampered_payload.value(QStringLiteral("preconditions")).toArray();
    auto filing_guard = guards.at(0).toObject();
    filing_guard.insert(QStringLiteral("accept_operation_id"),
                        QStringLiteral("test.operation.substituted"));
    guards.replace(0, filing_guard);
    tampered_payload.insert(QStringLiteral("preconditions"), guards);
    tampered.insert(QStringLiteral("payload"), tampered_payload);
    const auto tampered_decoded = storage::decodeWorkflowEvent(compact(tampered));
    QVERIFY(tampered_decoded.has_value());
    QVERIFY(*tampered_decoded != model::WorkflowEvent{event});

    auto generic_absent = event;
    generic_absent.header.preconditions = {
        model::WorkflowFilingPrecondition{model::FilingTypeId{"test.filing-type.opening"}, false},
        std::get<model::WorkflowFilingInstancePrecondition>(event.header.preconditions.at(0)),
    };
    auto conflict = storage::encodeWorkflowEvent(model::WorkflowEvent{generic_absent});
    QVERIFY(!conflict.has_value());
    std::ranges::reverse(generic_absent.header.preconditions);
    conflict = storage::encodeWorkflowEvent(model::WorkflowEvent{generic_absent});
    QVERIFY(!conflict.has_value());

    auto conflicting_envelope = envelope;
    tampered_payload = conflicting_envelope.value(QStringLiteral("payload")).toObject();
    guards = tampered_payload.value(QStringLiteral("preconditions")).toArray();
    guards.prepend(
        QJsonObject{{QStringLiteral("filing_type_id"), QStringLiteral("test.filing-type.opening")},
                    {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                    {QStringLiteral("present"), false}});
    tampered_payload.insert(QStringLiteral("preconditions"), guards);
    conflicting_envelope.insert(QStringLiteral("payload"), tampered_payload);
    rejected = storage::decodeWorkflowEvent(compact(conflicting_envelope));
    QVERIFY(!rejected.has_value());
}

void WorkflowCodecTest::rejectsStructuredVersionConfusionAndTampering() {
    const model::WorkflowCommand command = model::IssueWorkflowJudgment{
        commandHeader("test.command.structured-judgment"),
        model::WorkflowOperationId{"test.operation.judgment"}, std::string(64, 'f'),
        model::DispositionPlanId{"test.disposition.partial"}};
    auto command_envelope = commandObject(command);
    command_envelope.insert(QStringLiteral("schema_version"), 1);
    auto decoded_command = storage::decodeWorkflowCommand(compact(command_envelope));
    QVERIFY(!decoded_command.has_value());
    QCOMPARE(decoded_command.error().code, WorkflowCodecErrorCode::UnsupportedVersion);

    command_envelope = commandObject(commands().at(4));
    command_envelope.insert(QStringLiteral("schema_version"), 3);
    decoded_command = storage::decodeWorkflowCommand(compact(command_envelope));
    QVERIFY(!decoded_command.has_value());
    QCOMPARE(decoded_command.error().code, WorkflowCodecErrorCode::UnsupportedVersion);

    auto header = eventHeader("test.command.structured-judgment", "test.operation.judgment", 12);
    header.authority = authority(true);
    model::WorkflowEvent event =
        model::WorkflowJudgmentIssued{std::move(header), std::string(64, 'f'), dispositionPlan(),
                                      model::WorkflowStageId{"test.stage.mandate"}};
    auto event_envelope = eventObject(event);
    event_envelope.insert(QStringLiteral("schema_version"), 2);
    auto decoded_event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!decoded_event.has_value());

    event_envelope = eventObject(event);
    auto payload = event_envelope.value(QStringLiteral("payload")).toObject();
    payload.remove(QStringLiteral("preconditions"));
    event_envelope.insert(QStringLiteral("payload"), payload);
    decoded_event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!decoded_event.has_value());
    QCOMPARE(decoded_event.error().code, WorkflowCodecErrorCode::MissingField);

    auto guarded = std::get<model::WorkflowSealedSet>(events().at(6));
    guarded.header.authority = authority(true);
    guarded.header.preconditions = preconditions();
    event_envelope = eventObject(model::WorkflowEvent{guarded});
    payload = event_envelope.value(QStringLiteral("payload")).toObject();
    auto guards = payload.value(QStringLiteral("preconditions")).toArray();
    auto malformed = guards.first().toObject();
    malformed.insert(QStringLiteral("order_id"), QStringLiteral("test.order.injected"));
    guards.replace(0, malformed);
    payload.insert(QStringLiteral("preconditions"), guards);
    event_envelope.insert(QStringLiteral("payload"), payload);
    decoded_event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!decoded_event.has_value());
    QCOMPARE(decoded_event.error().code, WorkflowCodecErrorCode::UnexpectedField);

    event_envelope = eventObject(model::WorkflowEvent{guarded});
    payload = event_envelope.value(QStringLiteral("payload")).toObject();
    guards = payload.value(QStringLiteral("preconditions")).toArray();
    auto conflict = guards.at(2).toObject();
    conflict.insert(QStringLiteral("status"), QStringLiteral("satisfied"));
    guards.append(conflict);
    payload.insert(QStringLiteral("preconditions"), guards);
    event_envelope.insert(QStringLiteral("payload"), payload);
    decoded_event = storage::decodeWorkflowEvent(compact(event_envelope));
    QVERIFY(!decoded_event.has_value());
    QCOMPARE(decoded_event.error().code, WorkflowCodecErrorCode::InvalidField);

    guarded.header.authority = authority(false);
    const auto legacy_guarded = storage::encodeWorkflowEvent(model::WorkflowEvent{guarded});
    QVERIFY(!legacy_guarded.has_value());
    QCOMPARE(legacy_guarded.error().code, WorkflowCodecErrorCode::IncompleteAuthority);

    auto invalid_plan = dispositionPlan();
    invalid_plan.components.front().action = model::DispositionAction::Affirm;
    auto invalid_header =
        eventHeader("test.command.invalid-judgment", "test.operation.judgment", 13);
    invalid_header.authority = authority(true);
    const auto invalid_event = storage::encodeWorkflowEvent(model::WorkflowEvent{
        model::WorkflowJudgmentIssued{std::move(invalid_header), std::string(64, 'f'),
                                      std::move(invalid_plan), std::nullopt}});
    QVERIFY(!invalid_event.has_value());
    QCOMPARE(invalid_event.error().code, WorkflowCodecErrorCode::InvalidField);

    auto oversized_plan = dispositionPlan();
    while (oversized_plan.components.size() <= 32) {
        const auto index = oversized_plan.components.size();
        auto component = oversized_plan.components.front();
        component.issue_id.value = "test.issue.oversized-" + std::to_string(index);
        component.target_id.value = "test.target.oversized-" + std::to_string(index);
        oversized_plan.components.push_back(std::move(component));
    }
    invalid_header = eventHeader("test.command.oversized-judgment", "test.operation.judgment", 14);
    invalid_header.authority = authority(true);
    const auto oversized_event = storage::encodeWorkflowEvent(model::WorkflowEvent{
        model::WorkflowJudgmentIssued{std::move(invalid_header), std::string(64, 'f'),
                                      std::move(oversized_plan), std::nullopt}});
    QVERIFY(!oversized_event.has_value());
    QCOMPARE(oversized_event.error().code, WorkflowCodecErrorCode::OutOfRange);

    auto oversized_guards = std::get<model::WorkflowSealedSet>(events().at(6));
    oversized_guards.header.authority = authority(true);
    for (int index = 0; index < 33; ++index) {
        oversized_guards.header.preconditions.push_back(model::WorkflowFilingPrecondition{
            model::FilingTypeId{"test.filing-type.guard-" + std::to_string(index)}, true});
    }
    const auto oversized_guard_event =
        storage::encodeWorkflowEvent(model::WorkflowEvent{oversized_guards});
    QVERIFY(!oversized_guard_event.has_value());
    QCOMPARE(oversized_guard_event.error().code, WorkflowCodecErrorCode::OutOfRange);
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
