#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "main_window.hpp"
#include "record_workspace.hpp"

#include <QAction>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QMenu>
#include <QPdfSearchModel>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef APPELLATE_M4_SERRANO_ROOT
#error "APPELLATE_M4_SERRANO_ROOT must name content/m4/serrano-waiver"
#endif

#ifndef APPELLATE_M4_FOUNDATIONS
#error "APPELLATE_M4_FOUNDATIONS must name content/foundations"
#endif

namespace {

namespace model = appellate::model;
namespace packs = appellate::packs;
namespace ui = appellate::ui;

constexpr auto root_digest = "9b4941e97292faa0fceda1f1c719f6e38ce8478c82350c7fbbb74a010c27d344";
constexpr auto archive_digest = "d76686cec2053f78334c73f1c3aac415b637e733f0494b527001368597a1c243";
constexpr qint64 archive_byte_size = 3'453'568;
constexpr auto federal_digest = "866c90996c15e2076b9508a297ffce1a4e766b1432a9e11d08e8138c57e363c9";
constexpr auto ca4_digest = "449d75c77e5c47883f750377450f2d1ec1fc0e42e20b1f247446b208661d3262";
constexpr auto bench_digest = "cee0bf93309cc9ad800f215a47d734b20a9fdf5dc889f2f440e4382b942d332d";

[[nodiscard]] QByteArray sha256(QByteArrayView bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

class DeterministicRecordAccessProvider final : public ui::RecordAccessTransitionProvider {
  public:
    struct Transition final {
        QString session_id;
        std::uint64_t sequence{};
        std::string disclosure_id;
        model::RecordAccessAction action{};
    };

    [[nodiscard]] auto createdAtUtc(QStringView session_id)
        -> std::expected<QString, QString> override {
        created_sessions.push_back(session_id.toString());
        return QStringLiteral("2026-08-19T06:00:00Z");
    }

    [[nodiscard]] auto next(QStringView session_id, std::uint64_t next_sequence,
                            const packs::RuntimeRecordDisclosureId& disclosure_id,
                            model::RecordAccessAction action)
        -> std::expected<ui::RecordAccessTransitionStamp, QString> override {
        transitions.push_back(
            Transition{session_id.toString(), next_sequence, disclosure_id.value, action});
        return ui::RecordAccessTransitionStamp{
            QStringLiteral("ca4m4.serrano.record-access.event.%1")
                .arg(static_cast<qulonglong>(next_sequence)),
            QStringLiteral("2026-08-19T06:00:%1Z")
                .arg(static_cast<qulonglong>(next_sequence), 2, 10, QLatin1Char('0')),
        };
    }

    std::vector<QString> created_sessions;
    std::vector<Transition> transitions;
};

class SerranoWaiverUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void installsFinalPackAndExercisesPublicAndAuthorizedRecord();
};

void SerranoWaiverUiE2eTest::installsFinalPackAndExercisesPublicAndAuthorizedRecord() {
    const model::PackRevision expected_root{model::PackId{"us.ca4.m4.serrano-waiver"}, "1.2.0",
                                            root_digest};
    const model::PackRevision expected_federal{model::PackId{"foundation.us-federal"}, "2025.12.01",
                                               federal_digest};
    const model::PackRevision expected_ca4{model::PackId{"foundation.us-ca4"}, "2026.03.23",
                                           ca4_digest};
    const model::PackRevision expected_bench{model::PackId{"foundation.us-ca4-fictional-bench"},
                                             "1.0.0", bench_digest};
    const QDir root(QStringLiteral(APPELLATE_M4_SERRANO_ROOT));
    const QDir foundations(QStringLiteral(APPELLATE_M4_FOUNDATIONS));

    QTemporaryDir state;
    QVERIFY(state.isValid());
    const auto root_archive = QDir(state.path()).filePath(QStringLiteral("serrano-waiver.awpack"));
    const auto exported = packs::PackArchive::exportDirectory(
        root.filePath(QStringLiteral("pack-candidate")), root_archive, {},
        packs::PackValidationScope::ResolvedClosure);
    QVERIFY2(exported.has_value(), exported ? "" : qPrintable(exported.error().message));
    QCOMPARE(*exported, expected_root);
    QFile frozen_root(root_archive);
    QVERIFY2(frozen_root.open(QIODevice::ReadOnly), qPrintable(frozen_root.errorString()));
    QCOMPARE(frozen_root.size(), archive_byte_size);
    QCOMPARE(sha256(QByteArrayView(frozen_root.readAll())), QByteArray(archive_digest));

    const auto catalog_root = QDir(state.path()).filePath(QStringLiteral("catalog"));
    const auto access_database =
        QDir(state.path()).filePath(QStringLiteral("sessions/record-access.sqlite"));
    const auto catalog = packs::PackCatalog::open(catalog_root);
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));
    const auto federal = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-federal/foundation-us-federal-2025.12.01.awpack")),
        QStringLiteral("2026-08-19T05:59:00Z"));
    const auto ca4 = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral("us-ca4/foundation-us-ca4-2026.03.23.awpack")),
        QStringLiteral("2026-08-19T05:59:01Z"));
    const auto bench = (*catalog)->installArchive(
        foundations.filePath(QStringLiteral(
            "us-ca4-fictional-bench/foundation-us-ca4-fictional-bench-1.0.0.awpack")),
        QStringLiteral("2026-08-19T05:59:02Z"));
    QVERIFY2(federal.has_value(), federal ? "" : qPrintable(federal.error().message));
    QVERIFY2(ca4.has_value(), ca4 ? "" : qPrintable(ca4.error().message));
    QVERIFY2(bench.has_value(), bench ? "" : qPrintable(bench.error().message));
    QCOMPARE(federal->revision, expected_federal);
    QCOMPARE(ca4->revision, expected_ca4);
    QCOMPARE(bench->revision, expected_bench);
    const auto dependency_catalog = (*catalog)->list();
    QVERIFY2(dependency_catalog.has_value(),
             dependency_catalog ? "" : qPrintable(dependency_catalog.error().message));
    QCOMPARE(dependency_catalog->size(), std::size_t{3});

    auto access_provider = std::make_shared<DeterministicRecordAccessProvider>();
    ui::MainWindow window({}, catalog_root, nullptr, {}, access_provider, access_database);
    const auto installed = window.loadSource(root_archive);
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error()));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.currentRuntime() != nullptr);
    QCOMPARE(window.currentRuntime()->revision, expected_root);
    QCOMPARE(window.currentRuntime()->cases.size(), std::size_t{1});
    const auto& runtime_case = window.currentRuntime()->cases.front();
    QCOMPARE(runtime_case.definition.id.value, std::string("ca4m4.case.serrano-waiver"));
    QCOMPARE(runtime_case.record.id.value, std::string("ca4m4.serrano.record"));
    QCOMPARE(runtime_case.record.dockets.size(), std::size_t{3});
    QCOMPARE(runtime_case.record.docket_entries.size(), std::size_t{58});
    QCOMPARE(runtime_case.record.page_anchors.size(), std::size_t{456});
    QCOMPARE(runtime_case.record.sealed_disclosures.size(), std::size_t{8});
    QVERIFY(runtime_case.record.disclosure_policy.has_value());

    const auto opened = window.openSelectedRecord();
    QVERIFY2(opened.has_value(), opened ? "" : qPrintable(opened.error()));
    QCOMPARE(access_provider->created_sessions.size(), std::size_t{1});
    auto* workspace = window.recordWorkspace();
    QVERIFY(workspace != nullptr);
    auto* pdf_search = workspace->findChild<QPdfSearchModel*>();
    QVERIFY(pdf_search != nullptr);

    QCOMPARE(workspace->visibleDocketCount(), qsizetype{50});
    QHash<QString, qsizetype> derived_public_partition;
    for (const auto& entry : runtime_case.record.docket_entries) {
        if (!entry.sealed && entry.docket_id.has_value()) {
            ++derived_public_partition[QString::fromStdString(entry.docket_id->value)];
        }
    }
    QCOMPARE(derived_public_partition.value(QStringLiteral("ca4m4.serrano.docket.district")),
             qsizetype{24});
    QCOMPARE(derived_public_partition.value(QStringLiteral("ca4m4.serrano.docket.appellate")),
             qsizetype{16});
    QCOMPARE(
        derived_public_partition.value(QStringLiteral("ca4m4.serrano.docket.counterfactual-day15")),
        qsizetype{10});
    const std::array public_docket_partitions{
        QStringLiteral("ca4m4.serrano.docket.district"),
        QStringLiteral("ca4m4.serrano.docket.appellate"),
        QStringLiteral("ca4m4.serrano.docket.counterfactual-day15"),
    };
    for (const auto& filter : public_docket_partitions) {
        workspace->setDocketFilter(filter);
        QCOMPARE(workspace->visibleDocketCount(), derived_public_partition.value(filter));
    }
    const std::array public_tag_partitions{
        std::pair{QStringLiteral("lower_record"), qsizetype{24}},
        std::pair{QStringLiteral("actual_appellate_docket"), qsizetype{16}},
        std::pair{QStringLiteral("never_occurred_on_actual_docket"), qsizetype{10}},
    };
    for (const auto& [filter, expected_count] : public_tag_partitions) {
        workspace->setDocketFilter(filter);
        QCOMPARE(workspace->visibleDocketCount(), expected_count);
    }
    workspace->setDocketFilter(QStringLiteral("ca4m4.serrano.record.entry.a07"));
    QCOMPARE(workspace->visibleDocketCount(), qsizetype{0});
    workspace->setDocketFilter({});

    struct Navigation final {
        QString citation;
        QString entry_id;
        int page_count{};
        int page_index{};
        QString search;
    };
    const std::array public_navigations{
        Navigation{QStringLiteral("JA242"), QStringLiteral("ca4m4.serrano.record.entry.l24"), 5, 0,
                   QStringLiteral("imposed 36 months of imprisonment")},
        Navigation{QStringLiteral("PA133"), QStringLiteral("ca4m4.serrano.record.entry.a18"), 18, 0,
                   QStringLiteral("dismiss that part of the appeal")},
        Navigation{QStringLiteral("PA189"), QStringLiteral("ca4m4.serrano.record.entry.b09"), 6, 0,
                   QStringLiteral("government promptly invoked")},
    };
    for (const auto& navigation : public_navigations) {
        const auto navigated = workspace->navigateToCitation(navigation.citation);
        QVERIFY2(navigated.has_value(), navigated ? "" : qPrintable(navigated.error().message));
        QCOMPARE(workspace->currentDocumentId(), navigation.entry_id);
        QCOMPARE(workspace->loadedPageCount(), navigation.page_count);
        QTRY_COMPARE_WITH_TIMEOUT(workspace->currentPageIndex(), navigation.page_index, 10'000);
        workspace->setDocumentSearch(navigation.search);
        QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !pdf_search->resultsOnPage(workspace->currentPageIndex()).isEmpty(), 10'000);
        workspace->setDocumentSearch({});
    }

    constexpr auto disclosure_id = "ca4m4.serrano.disclosure.actual-opening-brief";
    const auto stable_anchor =
        QStringLiteral("ca4m4.serrano.anchor.stable.actual-opening-brief.page-01");
    const auto sealed_anchor = QStringLiteral("ca4m4.serrano.anchor.pa38");
    const auto public_entry = QStringLiteral("ca4m4.serrano.record.entry.a06");
    const auto sealed_entry = QStringLiteral("ca4m4.serrano.record.entry.a07");

    const auto public_twin = workspace->navigateToAnchor(stable_anchor);
    QVERIFY2(public_twin.has_value(), public_twin ? "" : qPrintable(public_twin.error().message));
    QCOMPARE(workspace->currentDocumentId(), public_entry);
    QCOMPARE(workspace->loadedPageCount(), 16);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->currentPageIndex(), 0, 10'000);
    const auto hidden_sealed = workspace->navigateToAnchor(sealed_anchor);
    QVERIFY(!hidden_sealed.has_value());
    QCOMPARE(hidden_sealed.error().code, ui::RecordWorkspaceErrorCode::InvalidPageAnchor);
    QCOMPARE(workspace->currentDocumentId(), public_entry);

    QCOMPARE(window.recordAccessMenu()->actions().size(), 8);
    auto* grant = window.findChild<QAction*>(
        QStringLiteral("grantRecordAccessAction.%1").arg(QString::fromLatin1(disclosure_id)));
    auto* revoke = window.findChild<QAction*>(
        QStringLiteral("revokeRecordAccessAction.%1").arg(QString::fromLatin1(disclosure_id)));
    QVERIFY(grant != nullptr);
    QVERIFY(revoke != nullptr);
    QVERIFY(grant->isEnabled());
    QVERIFY(!revoke->isEnabled());

    grant->trigger();
    QCOMPARE(access_provider->transitions.size(), std::size_t{1});
    QCOMPARE(access_provider->transitions.front().sequence, std::uint64_t{1});
    QCOMPARE(access_provider->transitions.front().disclosure_id, std::string(disclosure_id));
    QVERIFY(access_provider->transitions.front().action == model::RecordAccessAction::Grant);
    QVERIFY(!grant->isEnabled());
    QVERIFY(revoke->isEnabled());
    QCOMPARE(workspace->visibleDocketCount(), qsizetype{51});

    const auto sealed_twin = workspace->navigateToAnchor(stable_anchor);
    QVERIFY2(sealed_twin.has_value(), sealed_twin ? "" : qPrintable(sealed_twin.error().message));
    QCOMPARE(workspace->currentDocumentId(), sealed_entry);
    QCOMPARE(workspace->loadedPageCount(), 16);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->currentPageIndex(), 0, 10'000);
    workspace->setDocumentSearch(QStringLiteral("same sixteen subjects as the public brief"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(!pdf_search->resultsOnPage(0).isEmpty(), 10'000);

    revoke->trigger();
    QCOMPARE(access_provider->transitions.size(), std::size_t{2});
    QCOMPARE(access_provider->transitions.back().sequence, std::uint64_t{2});
    QCOMPARE(access_provider->transitions.back().disclosure_id, std::string(disclosure_id));
    QVERIFY(access_provider->transitions.back().action == model::RecordAccessAction::Revoke);
    QVERIFY(grant->isEnabled());
    QVERIFY(!revoke->isEnabled());
    QCOMPARE(workspace->visibleDocketCount(), qsizetype{50});
    QVERIFY(workspace->currentDocumentId().isEmpty());
    QCOMPARE(workspace->loadedPageCount(), 0);
    QCOMPARE(workspace->documentSearchResultCount(), 0);

    const auto restored_public_twin = workspace->navigateToAnchor(stable_anchor);
    QVERIFY2(restored_public_twin.has_value(),
             restored_public_twin ? "" : qPrintable(restored_public_twin.error().message));
    QCOMPARE(workspace->currentDocumentId(), public_entry);
    QCOMPARE(workspace->loadedPageCount(), 16);
    QTRY_COMPARE_WITH_TIMEOUT(workspace->currentPageIndex(), 0, 10'000);
    workspace->setDocumentSearch(
        QStringLiteral("This public version replaces protected presentence"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->documentSearchResultCount() > 0, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(!pdf_search->resultsOnPage(0).isEmpty(), 10'000);
}

} // namespace

QTEST_MAIN(SerranoWaiverUiE2eTest)

#include "tst_m4_serrano_waiver_ui_e2e.moc"
