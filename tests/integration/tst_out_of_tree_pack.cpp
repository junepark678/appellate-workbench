#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QDir>
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
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{1});

    const auto& argument = runtime_case.argument_configurations.front();
    QCOMPARE(argument.id.value, std::string("example.argument.fictional"));
    QCOMPARE(argument.bench.id.value, std::string("example.bench.fictional"));
    QCOMPARE(argument.bench.seats.size(), std::size_t{1});
    QCOMPARE(argument.bench.seats.front().profile_id.value, std::string("example.judge.fictional"));
    QCOMPARE(argument.bench.seats.front().profile.display_name, std::string("Judge Rowan"));

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

} // namespace

QTEST_GUILESS_MAIN(OutOfTreePackTest)
#include "tst_out_of_tree_pack.moc"
