#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

namespace engine = appellate::engine;
namespace model = appellate::model;
namespace packs = appellate::packs;

[[nodiscard]] QString fullPackPath() {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + QStringLiteral("/full-resource-pack");
}

[[nodiscard]] model::LegalDate date(int year, unsigned month, unsigned day) {
    return model::LegalDate{std::chrono::year{year} / std::chrono::month{month} /
                            std::chrono::day{day}};
}

[[nodiscard]] model::LegalTime at(model::LegalDate court_date) {
    return model::LegalTime{std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}},
                            court_date};
}

[[nodiscard]] model::WorkflowState initialState(const packs::RuntimeCase& runtime_case) {
    return model::WorkflowState{
        "example.session.runtime-pack",
        runtime_case.workflow.id,
        runtime_case.workflow.initial_stage_id,
        std::uint64_t{1},
        std::nullopt,
        {},
        {},
        {},
        {},
        {},
        false,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
}

[[nodiscard]] model::WorkflowCommand noticeCommand() {
    return model::SubmitWorkflowFiling{
        model::WorkflowCommandHeader{
            "example.session.runtime-pack",
            model::WorkflowCommandId{"example.command.runtime-notice"},
            model::ActorId{"example.actor.appellant"},
            at(date(2026, 1, 2)),
        },
        model::WorkflowFilingId{"example.filing.runtime-notice"},
        model::FilingTypeId{"example.filing.notice"},
        std::string(64, 'a'),
        {{model::FilingFieldId{"example.field.caption"}, "Example caption"}},
        {model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
}

class OutOfTreePackTest final : public QObject {
    Q_OBJECT

  private slots:
    void drivesBuiltInWorkflowFromInstalledArchive();
    void rejectsRemovedLinkedResource();
    void rejectsDuplicateAndMutatedResources();
    void validatesRouteOutcomesAndAllowsCatalogSuperset();
    void preservesRichRecordMetadataAndRejectsTampering();
};

void OutOfTreePackTest::drivesBuiltInWorkflowFromInstalledArchive() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto archive_path = QDir(temporary.path()).filePath(QStringLiteral("fictional.awpack"));
    const auto exported = packs::PackArchive::exportDirectory(fullPackPath(), archive_path);
    if (!exported) {
        QFAIL(qPrintable(exported.error().message));
    }
    const auto opened = packs::PackCatalog::open(
        QDir(temporary.path()).filePath(QStringLiteral("installed-packs")));
    if (!opened) {
        QFAIL(qPrintable(opened.error().message));
    }
    const auto installed =
        (*opened)->installArchive(archive_path, QStringLiteral("2026-08-11T00:00:00Z"));
    if (!installed) {
        QFAIL(qPrintable(installed.error().message));
    }
    const auto loaded = (*opened)->load(exported->id, exported->version);
    if (!loaded) {
        QFAIL(qPrintable(loaded.error().message));
    }

    const auto runtime = packs::loadRuntimePack(*loaded);
    if (!runtime) {
        QFAIL(runtime.error().message.c_str());
    }
    QCOMPARE(runtime->cases.size(), std::size_t{1});

    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.definition.id.value, std::string("example.case.fictional"));
    QCOMPARE(runtime_case.definition.procedure_id.value,
             std::string("example.procedure.fictional"));
    QCOMPARE(runtime_case.workflow.id.value, std::string("example.workflow.fictional"));
    QCOMPARE(runtime_case.record.id.value, std::string("example.record.fictional"));
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{1});
    QCOMPARE(runtime_case.record.docket_entries.front().id.value,
             std::string("example.record.entry-one"));
    QCOMPARE(runtime_case.record.docket_entries.front().page_count, std::uint32_t{3});
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{2});
    QCOMPARE(runtime_case.record.dockets.front().id.value,
             std::string("example.docket.district"));
    QCOMPARE(runtime_case.record.dockets.front().type, packs::RuntimeDocketType::District);
    QVERIFY(!runtime_case.record.dockets.front().court_id.has_value());
    QCOMPARE(runtime_case.record.dockets.at(1).court_id->value,
             std::string("example.court.fictional"));
    const auto& record_entry = runtime_case.record.docket_entries.front();
    QCOMPARE(record_entry.docket_id->value, std::string("example.docket.district"));
    QCOMPARE(*record_entry.entry_label, std::string("ECF No. 42"));
    QCOMPARE(*record_entry.actor, std::string("District clerk"));
    QCOMPARE(record_entry.tags,
             std::vector<std::string>({"appealable", "dispositive"}));
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{1});
    QCOMPARE(runtime_case.record.page_anchors.front().id.value,
             std::string("example.record.anchor.ja2"));
    QCOMPARE(runtime_case.record.page_anchors.front().page_number, std::uint32_t{2});
    QCOMPARE(*runtime_case.record.page_anchors.front().citation_label, std::string("JA2"));
    QCOMPARE(runtime_case.issues.front().record_anchor_ids.size(), std::size_t{2});
    QCOMPARE(runtime_case.issues.front().record_anchor_ids.at(1).value,
             std::string("example.record.anchor.ja2"));
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{1});

    const auto& argument = runtime_case.argument_configurations.front();
    QCOMPARE(argument.id.value, std::string("example.argument.fictional"));
    QCOMPARE(argument.bench.id.value, std::string("example.bench.fictional"));
    QCOMPARE(argument.bench.seats.size(), std::size_t{1});
    QCOMPARE(argument.bench.seats.front().profile_id.value, std::string("example.judge.fictional"));
    const auto& runtime_profile = argument.bench.seats.front().profile;
    QCOMPARE(runtime_profile.display_name, std::string("Composite Jurist Rowan"));
    QCOMPARE(runtime_profile.interaction.record_pin_demand, 0.88);
    QCOMPARE(runtime_profile.voice.question_framing, model::QuestionFraming::Direct);
    QCOMPARE(runtime_profile.voice.address_convention, model::CounselAddress::Counsel);
    QCOMPARE(runtime_profile.voice.question_phrases,
             std::vector<std::string>(
                 {"address the threshold question", "identify the governing rule"}));
    QCOMPARE(runtime_profile.voice.interruption_phrases,
             std::vector<std::string>({"pause at that premise", "before you move on"}));
    QCOMPARE(
        runtime_profile.voice.clarification_phrases,
        std::vector<std::string>({"clarify your position", "state the distinction precisely"}));

    const auto initial = initialState(runtime_case);
    const auto command = noticeCommand();
    const auto events =
        engine::decideWorkflow(runtime_case.workflow, runtime_case.definition, initial, command);
    if (!events) {
        QFAIL(events.error().message.c_str());
    }
    const auto stage_advanced =
        std::ranges::find_if(*events, [](const model::WorkflowEvent& event) {
            return std::holds_alternative<model::WorkflowStageAdvanced>(event);
        });
    QVERIFY(stage_advanced != events->end());
    QCOMPARE(std::get<model::WorkflowStageAdvanced>(*stage_advanced).next_stage_id.value,
             std::string("example.stage.submitted"));

    const std::vector journal{model::WorkflowJournalEntry{command, *events}};
    const auto replayed =
        engine::replayWorkflow(runtime_case.workflow, runtime_case.definition, initial, journal);
    if (!replayed) {
        QFAIL(replayed.error().message.c_str());
    }
    QCOMPARE(replayed->current_stage_id.value, std::string("example.stage.submitted"));
}

void OutOfTreePackTest::rejectsRemovedLinkedResource() {
    auto loaded = packs::PackReader::readDirectory(fullPackPath());
    QVERIFY(loaded.has_value());
    std::erase_if(loaded->resources, [](const packs::ValidatedResource& resource) {
        return resource.descriptor.kind == model::ResourceKind::Record;
    });

    const auto runtime = packs::loadRuntimePack(*loaded);
    QVERIFY(!runtime.has_value());
    QVERIFY(runtime.error().code == packs::RuntimePackErrorCode::MissingResource);
}

void OutOfTreePackTest::rejectsDuplicateAndMutatedResources() {
    auto duplicate = packs::PackReader::readDirectory(fullPackPath());
    QVERIFY(duplicate.has_value());
    const auto record =
        std::ranges::find_if(duplicate->resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::Record;
        });
    QVERIFY(record != duplicate->resources.end());
    duplicate->resources.push_back(*record);

    const auto duplicate_result = packs::loadRuntimePack(*duplicate);
    QVERIFY(!duplicate_result.has_value());
    QVERIFY(duplicate_result.error().code == packs::RuntimePackErrorCode::DuplicateResource);

    auto mutated = packs::PackReader::readDirectory(fullPackPath());
    QVERIFY(mutated.has_value());
    const auto workflow =
        std::ranges::find_if(mutated->resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::Workflow;
        });
    QVERIFY(workflow != mutated->resources.end());
    workflow->document.remove(QStringLiteral("operations"));

    const auto mutated_result = packs::loadRuntimePack(*mutated);
    QVERIFY(!mutated_result.has_value());
    QVERIFY(mutated_result.error().code == packs::RuntimePackErrorCode::InvalidResource);
}

void OutOfTreePackTest::validatesRouteOutcomesAndAllowsCatalogSuperset() {
    auto with_reference = packs::PackReader::readDirectory(fullPackPath());
    QVERIFY(with_reference.has_value());
    auto catalog = std::ranges::find_if(
        with_reference->resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::FilingCatalog;
        });
    QVERIFY(catalog != with_reference->resources.end());
    auto filings = catalog->document.value(QStringLiteral("filings")).toArray();
    filings.push_back(QJsonObject{
        {QStringLiteral("filing_id"), QStringLiteral("example.filing.reference-template")},
        {QStringLiteral("title"), QStringLiteral("Reference-only filing template")},
        {QStringLiteral("actor_role_ids"), QJsonArray{QStringLiteral("example.role.appellant")}},
        {QStringLiteral("required_field_ids"), QJsonArray{}},
        {QStringLiteral("authority_id"), QStringLiteral("example.authority.rule-one")},
    });
    catalog->document.insert(QStringLiteral("filings"), filings);
    const auto superset = packs::loadRuntimePack(*with_reference);
    QVERIFY2(superset.has_value(), superset ? "" : superset.error().message.c_str());

    for (int variant = 0; variant < 3; ++variant) {
        auto loaded = packs::PackReader::readDirectory(fullPackPath());
        QVERIFY(loaded.has_value());
        auto workflow =
            std::ranges::find_if(loaded->resources, [](const packs::ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::Workflow;
            });
        QVERIFY(workflow != loaded->resources.end());
        auto routes = workflow->document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        if (variant == 0) {
            route.insert(QStringLiteral("reject_operation_id"),
                         QStringLiteral("example.operation.accept-notice"));
        } else if (variant == 1) {
            route.insert(QStringLiteral("reject_operation_id"),
                         QStringLiteral("example.operation.missing"));
        } else {
            route.remove(QStringLiteral("deficiency_operation_id"));
        }
        routes.replace(0, route);
        workflow->document.insert(QStringLiteral("filing_routes"), routes);
        const auto result = packs::loadRuntimePack(*loaded);
        QVERIFY(!result.has_value());
        QVERIFY(result.error().code == packs::RuntimePackErrorCode::CrossReferenceFailure ||
                result.error().code == packs::RuntimePackErrorCode::InvalidResource);
    }
}

void OutOfTreePackTest::preservesRichRecordMetadataAndRejectsTampering() {
    auto loaded = packs::PackReader::readDirectory(fullPackPath());
    QVERIFY(loaded.has_value());
    auto record = std::ranges::find_if(
        loaded->resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::Record;
        });
    QVERIFY(record != loaded->resources.end());
    auto entries = record->document.value(QStringLiteral("docket_entries")).toArray();
    auto second = entries.at(0).toObject();
    second.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.entry-two"));
    second.insert(QStringLiteral("entry_number"), 2);
    second.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-1"));
    second.insert(QStringLiteral("parent_entry_id"),
                  QStringLiteral("example.record.entry-one"));
    second.insert(QStringLiteral("relationship"), QStringLiteral("attachment"));
    entries.push_back(second);
    record->document.insert(QStringLiteral("docket_entries"), entries);

    auto runtime = packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& attachment = runtime->cases.front().record.docket_entries.at(1);
    QCOMPARE(attachment.parent_entry_id->value, std::string("example.record.entry-one"));
    QCOMPARE(*attachment.relationship, packs::RuntimeRecordEntryRelationship::Attachment);

    auto cycle = *loaded;
    auto cycle_record = std::ranges::find_if(
        cycle.resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::Record;
        });
    auto cycle_entries = cycle_record->document.value(QStringLiteral("docket_entries")).toArray();
    auto first = cycle_entries.at(0).toObject();
    first.insert(QStringLiteral("parent_entry_id"),
                 QStringLiteral("example.record.entry-two"));
    first.insert(QStringLiteral("relationship"), QStringLiteral("component"));
    cycle_entries.replace(0, first);
    cycle_record->document.insert(QStringLiteral("docket_entries"), cycle_entries);
    const auto cycle_result = packs::loadRuntimePack(cycle);
    QVERIFY(!cycle_result.has_value());
    QCOMPARE(cycle_result.error().code, packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto bad_page = *loaded;
    auto bad_page_record = std::ranges::find_if(
        bad_page.resources, [](const packs::ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::Record;
        });
    auto anchors = bad_page_record->document.value(QStringLiteral("page_anchors")).toArray();
    auto anchor = anchors.at(0).toObject();
    anchor.insert(QStringLiteral("page_number"), 4);
    anchors.replace(0, anchor);
    bad_page_record->document.insert(QStringLiteral("page_anchors"), anchors);
    const auto page_result = packs::loadRuntimePack(bad_page);
    QVERIFY(!page_result.has_value());
    QCOMPARE(page_result.error().code, packs::RuntimePackErrorCode::CrossReferenceFailure);
}

} // namespace

QTEST_GUILESS_MAIN(OutOfTreePackTest)
#include "tst_out_of_tree_pack.moc"
