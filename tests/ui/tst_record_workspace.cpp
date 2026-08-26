#include "record_workspace.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPageSize>
#include <QPainter>
#include <QPdfView>
#include <QPdfWriter>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <cstddef>
#include <utility>

namespace appellate::ui {

class RecordWorkspaceTestAccess final {
  public:
    [[nodiscard]] static auto apply(RecordWorkspace& workspace,
                                    model::RecordAccessProjection projection)
        -> std::expected<void, RecordWorkspaceError> {
        return workspace.setAccessProjectionForTest(std::move(projection));
    }
};

} // namespace appellate::ui

namespace {

template <typename Workspace>
concept WorkspacePubliclyAcceptsAccessProjection =
    requires(Workspace& workspace, appellate::model::RecordAccessProjection projection) {
        workspace.setAccessProjection(std::move(projection));
    };

static_assert(!WorkspacePubliclyAcceptsAccessProjection<appellate::ui::RecordWorkspace>);

class RecordWorkspaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        if (!qEnvironmentVariableIsSet("APPELLATE_TEST_REQUIRE_2X_DPR")) {
            return;
        }

        QWidget probe;
        QCOMPARE(probe.devicePixelRatioF(), 2.0);
    }

    void filtersMetadataAndSearchesPdfText();
    void fixturePageCountMatchesRecordDeclaration();
    void navigatesStableAnchorAcrossMultipagePdf();
    void refusesSealedItem();
    void rejectsMissingAndOrphanDocuments();
    void rejectsInvalidDocketAndAnchorMetadata();
    void exposesKeyboardAccessibleControls();
    void largeDocketFilteringHasLinearWorkBudget();
    void projectsSealedTwinsWithoutMetadataLeaks();
    void defersSealedAssetUntilAuthorizedOpen();
    void revocationClosesSearchAndAllNavigationPaths();
};

using appellate::ui::RecordDefinition;
using appellate::ui::RecordDocketEntry;
using appellate::ui::RecordDocument;
using appellate::ui::RecordPageAnchor;
using appellate::ui::RecordWorkspace;
using appellate::ui::RecordWorkspaceErrorCode;

[[nodiscard]] bool writePdf(const QString& path, const QStringList& pages) {
    if (pages.isEmpty()) {
        return false;
    }
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::Letter));
    writer.setResolution(72);
    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    painter.setFont(QFont(QStringLiteral("DejaVu Sans"), 12));
    for (qsizetype index = 0; index < pages.size(); ++index) {
        painter.drawText(QRect(40, 40, 520, 700), Qt::AlignCenter, pages.at(index));
        if (index + 1 < pages.size() && !writer.newPage()) {
            return false;
        }
    }
    return painter.end();
}

[[nodiscard]] RecordDefinition basicRecord(const QString& pdf_path, bool sealed = false) {
    return RecordDefinition{
        {RecordDocument{
            QStringLiteral("doc.judgment"),
            QStringLiteral("Final Judgment"),
            pdf_path,
            sealed,
            {{QStringLiteral("court"), QStringLiteral("E.D. Virginia")},
             {QStringLiteral("record_type"), QStringLiteral("judgment")}},
            3,
        }},
        {
            RecordDocketEntry{
                QStringLiteral("docket.1"),
                QDate(2026, 8, 1),
                QStringLiteral("Final judgment entered"),
                QStringLiteral("District clerk"),
                QStringLiteral("Judgment following summary judgment briefing"),
                QStringLiteral("doc.judgment"),
                {QStringLiteral("dispositive"), QStringLiteral("appealable")},
                {{QStringLiteral("origin"), QStringLiteral("district court")}},
                QStringLiteral("docket.district"),
                QStringLiteral("1:26-cv-0042"),
                QStringLiteral("ECF No. 42"),
                {},
                {},
            },
            RecordDocketEntry{
                QStringLiteral("docket.2"),
                QDate(2026, 8, 2),
                QStringLiteral("Judgment transmitted"),
                QStringLiteral("Court operations"),
                QStringLiteral("Electronic transmission to all counsel"),
                QStringLiteral("doc.judgment"),
                {QStringLiteral("service")},
                {{QStringLiteral("channel"), QStringLiteral("electronic")}},
                QStringLiteral("docket.district"),
                QStringLiteral("1:26-cv-0042"),
                QStringLiteral("ECF No. 43"),
                QStringLiteral("docket.1"),
                QStringLiteral("attachment"),
            },
        },
        {
            RecordPageAnchor{QStringLiteral("record.judgment.p1"), QStringLiteral("doc.judgment"),
                             0, QStringLiteral("JA1")},
            RecordPageAnchor{QStringLiteral("record.judgment.p3"), QStringLiteral("doc.judgment"),
                             2, QStringLiteral("JA3")},
        },
        {appellate::ui::RecordDocketDescriptor{QStringLiteral("docket.district"),
                                               QStringLiteral("district"),
                                               {},
                                               QStringLiteral("E.D. Virginia"),
                                               QStringLiteral("1:26-cv-0042"),
                                               QStringLiteral("Example v. Example")}},
    };
}

[[nodiscard]] RecordDefinition twinRecord(const QString& public_pdf, const QString& sealed_pdf,
                                          int* resolution_count = nullptr,
                                          bool resolution_succeeds = true) {
    RecordDefinition definition{
        {
            RecordDocument{QStringLiteral("doc.redacted"),
                           QStringLiteral("Redacted filing"),
                           public_pdf,
                           false,
                           {{QStringLiteral("visibility"), QStringLiteral("public")}},
                           1},
            RecordDocument{
                QStringLiteral("doc.psr"),
                QStringLiteral("Confidential PSR title"),
                {},
                true,
                {{QStringLiteral("secret_tag"), QStringLiteral("ultrasecret-metadata")}},
                2,
                [sealed_pdf, resolution_count,
                 resolution_succeeds]() -> std::expected<appellate::ui::RecordAssetLease, QString> {
                    if (resolution_count != nullptr) {
                        ++*resolution_count;
                    }
                    if (!resolution_succeeds) {
                        return std::unexpected(QStringLiteral("sealed CAS verification failed"));
                    }
                    return appellate::ui::RecordAssetLease{sealed_pdf, {}};
                }},
        },
        {
            RecordDocketEntry{QStringLiteral("entry.redacted"),
                              QDate(2026, 8, 1),
                              QStringLiteral("Public redacted filing"),
                              QStringLiteral("Clerk"),
                              QStringLiteral("Public counterpart"),
                              QStringLiteral("doc.redacted"),
                              {QStringLiteral("redacted")},
                              {},
                              QStringLiteral("docket.one"),
                              QStringLiteral("26-1001"),
                              QStringLiteral("ECF 10"),
                              {},
                              {}},
            RecordDocketEntry{QStringLiteral("entry.psr"),
                              QDate(2026, 8, 1),
                              QStringLiteral("Sealed PSR secret title"),
                              QStringLiteral("Probation office"),
                              QStringLiteral("Never disclose this description"),
                              QStringLiteral("doc.psr"),
                              {QStringLiteral("psr-secret-tag")},
                              {},
                              QStringLiteral("docket.one"),
                              QStringLiteral("26-1001"),
                              QStringLiteral("ECF 10-S"),
                              {},
                              {}},
        },
        {
            RecordPageAnchor{QStringLiteral("anchor.public"), QStringLiteral("doc.redacted"), 0,
                             QStringLiteral("PUB-JA-10")},
            RecordPageAnchor{QStringLiteral("anchor.sealed"), QStringLiteral("doc.psr"), 1,
                             QStringLiteral("SECRET-JA-10")},
        },
        {appellate::ui::RecordDocketDescriptor{QStringLiteral("docket.one"),
                                               QStringLiteral("appellate"),
                                               {},
                                               QStringLiteral("Fictional Circuit"),
                                               QStringLiteral("26-1001"),
                                               QStringLiteral("Composite v. Fictional")}},
    };
    definition.disclosure_policy = appellate::ui::RecordDisclosurePolicy{
        QStringLiteral("record.twins"), QStringLiteral("record.policy.twins"),
        QStringLiteral("public_counterparts_only"), QStringLiteral("public_and_authorized_sealed"),
        QStringLiteral("session_event_grant_required")};
    definition.sealed_disclosures.push_back(appellate::ui::RecordSealedDisclosure{
        QStringLiteral("disclosure.public.psr"),
        QStringLiteral("doc.psr"),
        QStringLiteral("doc.redacted"),
        {},
        {},
        QStringLiteral("authority.psr-access"),
        {QStringLiteral("motion"), QStringLiteral("certificate"),
         QStringLiteral("redacted_counterpart")},
        {appellate::ui::RecordTwinAnchor{QStringLiteral("anchor.stable.psr"),
                                         QStringLiteral("anchor.sealed"),
                                         QStringLiteral("anchor.public")}}});
    return definition;
}

void RecordWorkspaceTest::filtersMetadataAndSearchesPdfText() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto pdf = QDir(directory.path()).filePath(QStringLiteral("record.pdf"));
    QVERIFY(writePdf(pdf, {QStringLiteral("Opening page"),
                           QStringLiteral("Unique second-page evidentiary text"),
                           QStringLiteral("Closing page")}));

    RecordWorkspace workspace;
    const auto loaded = workspace.setRecord(basicRecord(pdf));
    QVERIFY(loaded.has_value());

    workspace.setDocketFilter(QStringLiteral("district clerk summary judgment"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocketFilter(QStringLiteral("electronic service"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocketFilter(QStringLiteral("virginia record_type"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{2});
    workspace.setDocketFilter(QStringLiteral("1:26-cv-0042 ECF No. 43 attachment"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocketFilter({});
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{2});

    const auto opened = workspace.openDocketEntry(QStringLiteral("docket.1"));
    QVERIFY(opened.has_value());
    workspace.setDocumentSearch(QStringLiteral("evidentiary"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() >= 1, 10'000);
}

void RecordWorkspaceTest::fixturePageCountMatchesRecordDeclaration() {
    const auto pdf = QFINDTESTDATA("../fixtures/full-resource-pack/objects/final-order.pdf");
    const auto record_path = QFINDTESTDATA("../fixtures/full-resource-pack/resources/record.json");
    QVERIFY2(!pdf.isEmpty(), "Cannot locate the full-pack PDF fixture");
    QVERIFY2(!record_path.isEmpty(), "Cannot locate the full-pack record fixture");
    QFile record_file(record_path);
    QVERIFY(record_file.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    const auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    QCOMPARE(entries.size(), 1);
    const auto declared_page_count =
        entries.at(0).toObject().value(QStringLiteral("page_count")).toInt();

    RecordWorkspace workspace;
    QVERIFY(workspace.setRecord(basicRecord(pdf)).has_value());
    QVERIFY(workspace.openDocketEntry(QStringLiteral("docket.1")).has_value());
    QCOMPARE(workspace.loadedPageCount(), declared_page_count);
    QCOMPARE(declared_page_count, 3);
}

void RecordWorkspaceTest::navigatesStableAnchorAcrossMultipagePdf() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto pdf = QDir(directory.path()).filePath(QStringLiteral("record.pdf"));
    QVERIFY(writePdf(pdf, {QStringLiteral("Page one"), QStringLiteral("Page two"),
                           QStringLiteral("Page three")}));

    RecordWorkspace workspace;
    QVERIFY(workspace.setRecord(basicRecord(pdf)).has_value());
    const auto navigated = workspace.navigateToAnchor(QStringLiteral("record.judgment.p3"));
    QVERIFY(navigated.has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("doc.judgment"));
    QCOMPARE(workspace.loadedPageCount(), 3);
    QTRY_COMPARE(workspace.currentPageIndex(), 2);

    QVERIFY(workspace.navigateToCitation(QStringLiteral("JA1")).has_value());
    QTRY_COMPARE(workspace.currentPageIndex(), 0);
    const auto missing_citation = workspace.navigateToCitation(QStringLiteral("JA404"));
    QVERIFY(!missing_citation.has_value());
    QCOMPARE(missing_citation.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);

    QVERIFY(workspace.goToPage(1).has_value());
    QTRY_COMPARE(workspace.currentPageIndex(), 1);
}

void RecordWorkspaceTest::refusesSealedItem() {
    RecordWorkspace workspace;
    auto definition = basicRecord({}, true);
    QVERIFY(workspace.setRecord(std::move(definition)).has_value());

    const auto opened = workspace.openDocketEntry(QStringLiteral("docket.1"));
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error().code, RecordWorkspaceErrorCode::SealedDocument);
    QVERIFY(workspace.currentDocumentId().isEmpty());
    QCOMPARE(workspace.loadedPageCount(), 0);
}

void RecordWorkspaceTest::rejectsMissingAndOrphanDocuments() {
    RecordWorkspace workspace;
    RecordDefinition missing{
        {},
        {RecordDocketEntry{
            QStringLiteral("docket.missing"),
            QDate(2026, 8, 1),
            QStringLiteral("Missing"),
            QStringLiteral("Clerk"),
            QStringLiteral("Missing document reference"),
            QStringLiteral("doc.missing"),
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        }},
        {},
        {},
    };
    const auto missing_result = workspace.setRecord(std::move(missing));
    QVERIFY(!missing_result.has_value());
    QCOMPARE(missing_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    RecordDefinition orphan{
        {RecordDocument{QStringLiteral("doc.orphan"), QStringLiteral("Orphan"), {}, true, {}, 0}},
        {RecordDocketEntry{
            QStringLiteral("docket.other"),
            QDate(2026, 8, 1),
            QStringLiteral("Other"),
            QStringLiteral("Clerk"),
            QStringLiteral("References another document"),
            QStringLiteral("doc.other"),
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        }},
        {},
        {},
    };
    const auto orphan_result = workspace.setRecord(std::move(orphan));
    QVERIFY(!orphan_result.has_value());
    QCOMPARE(orphan_result.error().code, RecordWorkspaceErrorCode::MissingDocument);

    RecordDefinition true_orphan{
        {RecordDocument{QStringLiteral("doc.orphan"), QStringLiteral("Orphan"), {}, true, {}, 0}},
        {RecordDocketEntry{
            QStringLiteral("docket.only"),
            QDate(2026, 8, 1),
            QStringLiteral("Only"),
            QStringLiteral("Clerk"),
            QStringLiteral("No document intentionally"),
            QStringLiteral("doc.orphan"),
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        }},
        {},
        {},
    };
    true_orphan.documents.push_back(
        RecordDocument{QStringLiteral("doc.unused"), QStringLiteral("Unused"), {}, true, {}, 0});
    const auto true_orphan_result = workspace.setRecord(std::move(true_orphan));
    QVERIFY(!true_orphan_result.has_value());
    QCOMPARE(true_orphan_result.error().code, RecordWorkspaceErrorCode::OrphanDocument);
}

void RecordWorkspaceTest::rejectsInvalidDocketAndAnchorMetadata() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto pdf = QDir(directory.path()).filePath(QStringLiteral("record.pdf"));
    QVERIFY(writePdf(pdf, {QStringLiteral("Page one"), QStringLiteral("Page two"),
                           QStringLiteral("Page three")}));

    RecordWorkspace workspace;
    auto orphan_docket = basicRecord(pdf);
    orphan_docket.docket.front().docket_id = QStringLiteral("docket.missing");
    const auto docket_result = workspace.setRecord(std::move(orphan_docket));
    QVERIFY(!docket_result.has_value());
    QCOMPARE(docket_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto out_of_range = basicRecord(pdf);
    out_of_range.anchors.front().page_index = 3;
    const auto page_result = workspace.setRecord(std::move(out_of_range));
    QVERIFY(!page_result.has_value());
    QCOMPARE(page_result.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);

    auto duplicate_citation = basicRecord(pdf);
    duplicate_citation.anchors.at(1).citation_label = QStringLiteral("JA1");
    const auto citation_result = workspace.setRecord(std::move(duplicate_citation));
    QVERIFY(!citation_result.has_value());
    QCOMPARE(citation_result.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);

    auto entry_anchor_collision = basicRecord(pdf);
    entry_anchor_collision.anchors.front().id = QStringLiteral("docket.1");
    const auto collision_result = workspace.setRecord(std::move(entry_anchor_collision));
    QVERIFY(!collision_result.has_value());
    QCOMPARE(collision_result.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);

    auto unpaired_parent = basicRecord(pdf);
    unpaired_parent.docket.at(1).relationship.clear();
    const auto unpaired_result = workspace.setRecord(std::move(unpaired_parent));
    QVERIFY(!unpaired_result.has_value());
    QCOMPARE(unpaired_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto orphan_parent = basicRecord(pdf);
    orphan_parent.docket.at(1).parent_entry_id = QStringLiteral("docket.missing");
    const auto parent_result = workspace.setRecord(std::move(orphan_parent));
    QVERIFY(!parent_result.has_value());
    QCOMPARE(parent_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto cross_docket_parent = basicRecord(pdf);
    cross_docket_parent.dockets.push_back(
        appellate::ui::RecordDocketDescriptor{QStringLiteral("docket.appellate"),
                                              QStringLiteral("appellate"),
                                              {},
                                              QStringLiteral("Fourth Circuit"),
                                              QStringLiteral("26-1001"),
                                              QStringLiteral("Example v. Example")});
    cross_docket_parent.docket.at(1).docket_id = QStringLiteral("docket.appellate");
    const auto cross_docket_result = workspace.setRecord(std::move(cross_docket_parent));
    QVERIFY(!cross_docket_result.has_value());
    QCOMPARE(cross_docket_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto cycle = basicRecord(pdf);
    cycle.docket.front().parent_entry_id = QStringLiteral("docket.2");
    cycle.docket.front().relationship = QStringLiteral("component");
    const auto cycle_result = workspace.setRecord(std::move(cycle));
    QVERIFY(!cycle_result.has_value());
    QCOMPARE(cycle_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto public_to_sealed_parent = twinRecord(pdf, pdf);
    public_to_sealed_parent.docket.front().parent_entry_id = QStringLiteral("entry.psr");
    public_to_sealed_parent.docket.front().relationship = QStringLiteral("supplement");
    const auto sealed_parent_result = workspace.setRecord(std::move(public_to_sealed_parent));
    QVERIFY(!sealed_parent_result.has_value());
    QCOMPARE(sealed_parent_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto ordered_identity_collision = twinRecord(pdf, pdf);
    auto second_sealed_document = ordered_identity_collision.documents.back();
    second_sealed_document.id = QStringLiteral("doc.second-sealed");
    second_sealed_document.title = QStringLiteral("Second sealed filing");
    ordered_identity_collision.documents.push_back(std::move(second_sealed_document));
    auto second_sealed_entry = ordered_identity_collision.docket.back();
    second_sealed_entry.id = QStringLiteral("entry.second-sealed");
    second_sealed_entry.document_id = QStringLiteral("doc.second-sealed");
    second_sealed_entry.entry_label = QStringLiteral("ECF 11-S");
    ordered_identity_collision.docket.push_back(std::move(second_sealed_entry));
    auto second_disclosure = ordered_identity_collision.sealed_disclosures.front();
    second_disclosure.disclosure_id = QStringLiteral("anchor.stable.psr");
    second_disclosure.sealed_document_id = QStringLiteral("doc.second-sealed");
    second_disclosure.public_document_id.clear();
    second_disclosure.required_items.clear();
    second_disclosure.anchor_mappings.clear();
    ordered_identity_collision.sealed_disclosures.push_back(std::move(second_disclosure));
    const auto identity_collision_result =
        workspace.setRecord(std::move(ordered_identity_collision));
    QVERIFY(!identity_collision_result.has_value());
    QCOMPARE(identity_collision_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    auto hangul_boundary = basicRecord(pdf);
    hangul_boundary.docket.front().actor = QString(240, QChar(0xD55C));
    QVERIFY(workspace.setRecord(std::move(hangul_boundary)).has_value());
    auto hangul_overflow = basicRecord(pdf);
    hangul_overflow.docket.front().actor = QString(241, QChar(0xD55C));
    const auto hangul_result = workspace.setRecord(std::move(hangul_overflow));
    QVERIFY(!hangul_result.has_value());
    QCOMPARE(hangul_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);
}

void RecordWorkspaceTest::exposesKeyboardAccessibleControls() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto pdf = QDir(directory.path()).filePath(QStringLiteral("record.pdf"));
    QVERIFY(writePdf(pdf, {QStringLiteral("Page one"), QStringLiteral("Page two"),
                           QStringLiteral("Page three")}));

    RecordWorkspace workspace;
    workspace.resize(1000, 700);
    workspace.show();
    QVERIFY(workspace.setRecord(basicRecord(pdf)).has_value());
    QApplication::processEvents();

    QVERIFY(!workspace.docketFilterEdit()->accessibleName().isEmpty());
    QVERIFY(!workspace.documentSearchEdit()->accessibleName().isEmpty());
    QVERIFY(!workspace.docketView()->accessibleName().isEmpty());
    QVERIFY(!workspace.pdfView()->accessibleName().isEmpty());
    QVERIFY(!workspace.openDocumentButton()->accessibleName().isEmpty());
    QVERIFY(!workspace.previousPageButton()->accessibleName().isEmpty());
    QVERIFY(!workspace.nextPageButton()->accessibleName().isEmpty());
    QVERIFY(!workspace.pageSpinBox()->accessibleName().isEmpty());
    QVERIFY(workspace.docketFilterEdit()->focusPolicy() != Qt::NoFocus);
    QVERIFY(workspace.docketView()->focusPolicy() != Qt::NoFocus);
    QVERIFY(workspace.pdfView()->focusPolicy() != Qt::NoFocus);

    workspace.docketView()->setFocus();
    QTest::keyClick(workspace.docketView(), Qt::Key_F, Qt::ControlModifier);
    QTRY_VERIFY(workspace.docketFilterEdit()->hasFocus());

    workspace.setDocketFilter({});
    workspace.docketView()->setCurrentIndex(workspace.docketView()->model()->index(0, 0));
    workspace.docketView()->setFocus();
    QTest::keyClick(workspace.docketView(), Qt::Key_Return);
    QTRY_COMPARE(workspace.currentDocumentId(), QStringLiteral("doc.judgment"));
    QTRY_COMPARE(workspace.currentPageIndex(), 0);

    QTest::keyClick(workspace.pdfView(), Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE(workspace.currentPageIndex(), 1);
}

void RecordWorkspaceTest::largeDocketFilteringHasLinearWorkBudget() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto pdf = QDir(directory.path()).filePath(QStringLiteral("record.pdf"));
    QVERIFY(writePdf(pdf, {QStringLiteral("Large docket record")}));

    constexpr qsizetype entry_count = 10'000;
    RecordDefinition definition{
        {RecordDocument{
            QStringLiteral("doc.large"), QStringLiteral("Large record"), pdf, false, {}, 1}},
        {},
        {},
        {},
    };
    definition.docket.reserve(static_cast<std::size_t>(entry_count));
    for (qsizetype index = 0; index < entry_count; ++index) {
        definition.docket.push_back(RecordDocketEntry{
            QStringLiteral("docket.%1").arg(index),
            QDate(2026, 8, 1),
            QStringLiteral("Entry %1").arg(index),
            QStringLiteral("Clerk"),
            QStringLiteral("search-token-%1").arg(index),
            QStringLiteral("doc.large"),
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        });
    }

    RecordWorkspace workspace;
    QVERIFY(workspace.setRecord(std::move(definition)).has_value());
    workspace.setDocketFilter(QStringLiteral("search-token-9999"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    QVERIFY2(workspace.filterEvaluationCount() <= entry_count,
             "Filtering exceeded the deterministic one-row-inspection-per-entry budget");
}

void RecordWorkspaceTest::projectsSealedTwinsWithoutMetadataLeaks() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto public_pdf = directory.filePath(QStringLiteral("public.pdf"));
    const auto sealed_pdf = directory.filePath(QStringLiteral("sealed.pdf"));
    QVERIFY(writePdf(public_pdf, {QStringLiteral("Public redacted text")}));
    QVERIFY(writePdf(sealed_pdf, {QStringLiteral("Sealed first page"),
                                  QStringLiteral("ultrasecret-document-text")}));

    int sealed_resolution_count = 0;
    RecordWorkspace workspace;
    QVERIFY(workspace.setRecord(twinRecord(public_pdf, sealed_pdf, &sealed_resolution_count))
                .has_value());
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    QCOMPARE(sealed_resolution_count, 0);

    for (const auto& query :
         {QStringLiteral("Confidential PSR title"), QStringLiteral("psr-secret-tag"),
          QStringLiteral("ultrasecret-metadata"), QStringLiteral("SECRET-JA-10")}) {
        workspace.setDocketFilter(query);
        QCOMPARE(workspace.visibleDocketCount(), qsizetype{0});
    }
    workspace.setDocketFilter({});
    const auto raw_sealed = workspace.navigateToAnchor(QStringLiteral("anchor.sealed"));
    QVERIFY(!raw_sealed.has_value());
    QCOMPARE(raw_sealed.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);
    const auto sealed_citation = workspace.navigateToCitation(QStringLiteral("SECRET-JA-10"));
    QVERIFY(!sealed_citation.has_value());
    QCOMPARE(sealed_citation.error().code, RecordWorkspaceErrorCode::InvalidPageAnchor);
    QVERIFY(workspace.navigateToAnchor(QStringLiteral("anchor.stable.psr")).has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("doc.redacted"));
    QCOMPARE(workspace.loadedPageCount(), 1);
    QCOMPARE(sealed_resolution_count, 0);

    const auto deficiencies = workspace.disclosureDeficiencies();
    QCOMPARE(deficiencies.size(), std::size_t{2});
    QCOMPARE(deficiencies.at(0).kind,
             appellate::model::RecordDisclosureDeficiencyKind::MissingPublicMotion);
    QCOMPARE(deficiencies.at(1).kind,
             appellate::model::RecordDisclosureDeficiencyKind::MissingCertificate);
    QCOMPARE(deficiencies.at(0).disclosure_id, std::string("disclosure.public.psr"));
    QCOMPARE(deficiencies.at(1).disclosure_id, std::string("disclosure.public.psr"));
}

void RecordWorkspaceTest::defersSealedAssetUntilAuthorizedOpen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto public_pdf = directory.filePath(QStringLiteral("public.pdf"));
    QVERIFY(writePdf(public_pdf, {QStringLiteral("Public redacted text")}));

    int sealed_resolution_count = 0;
    RecordWorkspace workspace;
    QVERIFY(workspace
                .setRecord(twinRecord(public_pdf, QStringLiteral("not-exposed-sealed.pdf"),
                                      &sealed_resolution_count, false))
                .has_value());

    // Public projection, direct IDs, citations, and stable public anchors must
    // not touch the deferred sealed asset capability.
    QVERIFY(!workspace.openDocketEntry(QStringLiteral("entry.psr")).has_value());
    QVERIFY(!workspace.navigateToAnchor(QStringLiteral("anchor.sealed")).has_value());
    QVERIFY(!workspace.navigateToCitation(QStringLiteral("SECRET-JA-10")).has_value());
    QVERIFY(workspace.navigateToAnchor(QStringLiteral("anchor.stable.psr")).has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("doc.redacted"));
    QCOMPARE(sealed_resolution_count, 0);

    appellate::model::RecordAccessProjection granted{"session.local.corrupt", "record.twins",
                                                     "record.policy.twins",   1,
                                                     std::string(64, 'a'),    {"doc.psr"}};
    QVERIFY(
        appellate::ui::RecordWorkspaceTestAccess::apply(workspace, std::move(granted)).has_value());
    QCOMPARE(sealed_resolution_count, 0);
    const auto sealed_open = workspace.openDocketEntry(QStringLiteral("entry.psr"));
    QVERIFY(!sealed_open.has_value());
    QCOMPARE(sealed_open.error().code, RecordWorkspaceErrorCode::PdfLoadFailed);
    QVERIFY(!sealed_open.error().message.contains(QStringLiteral("doc.psr")));
    QVERIFY(!sealed_open.error().message.contains(QStringLiteral("Confidential")));
    QCOMPARE(sealed_resolution_count, 1);
    QVERIFY(workspace.currentDocumentId().isEmpty());

    appellate::model::RecordAccessProjection revoked{"session.local.corrupt", "record.twins",
                                                     "record.policy.twins",   2,
                                                     std::string(64, 'b'),    {}};
    QVERIFY(
        appellate::ui::RecordWorkspaceTestAccess::apply(workspace, std::move(revoked)).has_value());
    QVERIFY(workspace.navigateToAnchor(QStringLiteral("anchor.stable.psr")).has_value());
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("doc.redacted"));
    QCOMPARE(sealed_resolution_count, 1);
}

void RecordWorkspaceTest::revocationClosesSearchAndAllNavigationPaths() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto public_pdf = directory.filePath(QStringLiteral("public.pdf"));
    const auto sealed_pdf = directory.filePath(QStringLiteral("sealed.pdf"));
    QVERIFY(writePdf(public_pdf, {QStringLiteral("Public redacted text")}));
    QVERIFY(writePdf(sealed_pdf, {QStringLiteral("Sealed first page"),
                                  QStringLiteral("ultrasecret-document-text")}));

    int sealed_resolution_count = 0;
    RecordWorkspace workspace;
    workspace.resize(1000, 700);
    workspace.show();
    QVERIFY(workspace.setRecord(twinRecord(public_pdf, sealed_pdf, &sealed_resolution_count))
                .has_value());
    const auto unauthorized_direct = workspace.openDocketEntry(QStringLiteral("entry.psr"));
    QVERIFY(!unauthorized_direct.has_value());
    QCOMPARE(unauthorized_direct.error().code, RecordWorkspaceErrorCode::MissingDocketEntry);
    QCOMPARE(sealed_resolution_count, 0);

    appellate::model::RecordAccessProjection granted{"session.local.one",   "record.twins",
                                                     "record.policy.twins", 1,
                                                     std::string(64, 'a'),  {"doc.psr"}};
    QVERIFY(
        appellate::ui::RecordWorkspaceTestAccess::apply(workspace, std::move(granted)).has_value());
    QCOMPARE(sealed_resolution_count, 0);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{2});
    workspace.setDocketFilter(QStringLiteral("psr-secret-tag"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocketFilter({});
    QVERIFY(workspace.navigateToAnchor(QStringLiteral("anchor.stable.psr")).has_value());
    QCOMPARE(sealed_resolution_count, 1);
    QCOMPARE(workspace.currentDocumentId(), QStringLiteral("doc.psr"));
    QTRY_COMPARE(workspace.currentPageIndex(), 1);
    workspace.setDocumentSearch(QStringLiteral("ultrasecret-document-text"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() >= 1, 10'000);

    appellate::model::RecordAccessProjection revoked{
        "session.local.one", "record.twins", "record.policy.twins", 2, std::string(64, 'b'), {}};
    QVERIFY(
        appellate::ui::RecordWorkspaceTestAccess::apply(workspace, std::move(revoked)).has_value());
    QVERIFY(workspace.currentDocumentId().isEmpty());
    QCOMPARE(workspace.loadedPageCount(), 0);
    QCOMPARE(workspace.documentSearchResultCount(), 0);
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    QVERIFY(!workspace.navigateToAnchor(QStringLiteral("anchor.sealed")).has_value());
    QVERIFY(!workspace.navigateToCitation(QStringLiteral("SECRET-JA-10")).has_value());
    QCOMPARE(sealed_resolution_count, 1);

    appellate::model::RecordAccessProjection stale_grant{"session.local.one",   "record.twins",
                                                         "record.policy.twins", 1,
                                                         std::string(64, 'a'),  {"doc.psr"}};
    const auto rollback =
        appellate::ui::RecordWorkspaceTestAccess::apply(workspace, std::move(stale_grant));
    QVERIFY(!rollback.has_value());
    QCOMPARE(rollback.error().code, RecordWorkspaceErrorCode::AccessProjectionMismatch);
    QVERIFY(workspace.currentDocumentId().isEmpty());

    workspace.docketView()->setCurrentIndex(workspace.docketView()->model()->index(0, 0));
    workspace.docketView()->setFocus();
    QTest::keyClick(workspace.docketView(), Qt::Key_Return);
    QTRY_COMPARE(workspace.currentDocumentId(), QStringLiteral("doc.redacted"));
    QVERIFY(workspace.goToPage(1).has_value() == false);
}

} // namespace

QTEST_MAIN(RecordWorkspaceTest)

#include "tst_record_workspace.moc"
