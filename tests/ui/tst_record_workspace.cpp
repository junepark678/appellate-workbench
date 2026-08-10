#include "record_workspace.hpp"

#include <QApplication>
#include <QDir>
#include <QFont>
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

#include <cstddef>
#include <utility>

namespace {

class RecordWorkspaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void filtersMetadataAndSearchesPdfText();
    void navigatesStableAnchorAcrossMultipagePdf();
    void refusesSealedItem();
    void rejectsMissingAndOrphanDocuments();
    void exposesKeyboardAccessibleControls();
    void largeDocketFilteringHasLinearWorkBudget();
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
            },
        },
        {
            RecordPageAnchor{QStringLiteral("record.judgment.p1"), QStringLiteral("doc.judgment"),
                             0},
            RecordPageAnchor{QStringLiteral("record.judgment.p3"), QStringLiteral("doc.judgment"),
                             2},
        },
    };
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
    workspace.setDocketFilter({});
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{2});

    const auto opened = workspace.openDocketEntry(QStringLiteral("docket.1"));
    QVERIFY(opened.has_value());
    workspace.setDocumentSearch(QStringLiteral("evidentiary"));
    QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() >= 1, 10'000);
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
        }},
        {},
    };
    const auto missing_result = workspace.setRecord(std::move(missing));
    QVERIFY(!missing_result.has_value());
    QCOMPARE(missing_result.error().code, RecordWorkspaceErrorCode::InvalidDefinition);

    RecordDefinition orphan{
        {RecordDocument{QStringLiteral("doc.orphan"), QStringLiteral("Orphan"), {}, true, {}}},
        {RecordDocketEntry{
            QStringLiteral("docket.other"),
            QDate(2026, 8, 1),
            QStringLiteral("Other"),
            QStringLiteral("Clerk"),
            QStringLiteral("References another document"),
            QStringLiteral("doc.other"),
            {},
            {},
        }},
        {},
    };
    const auto orphan_result = workspace.setRecord(std::move(orphan));
    QVERIFY(!orphan_result.has_value());
    QCOMPARE(orphan_result.error().code, RecordWorkspaceErrorCode::MissingDocument);

    RecordDefinition true_orphan{
        {RecordDocument{QStringLiteral("doc.orphan"), QStringLiteral("Orphan"), {}, true, {}}},
        {RecordDocketEntry{
            QStringLiteral("docket.only"),
            QDate(2026, 8, 1),
            QStringLiteral("Only"),
            QStringLiteral("Clerk"),
            QStringLiteral("No document intentionally"),
            QStringLiteral("doc.orphan"),
            {},
            {},
        }},
        {},
    };
    true_orphan.documents.push_back(
        RecordDocument{QStringLiteral("doc.unused"), QStringLiteral("Unused"), {}, true, {}});
    const auto true_orphan_result = workspace.setRecord(std::move(true_orphan));
    QVERIFY(!true_orphan_result.has_value());
    QCOMPARE(true_orphan_result.error().code, RecordWorkspaceErrorCode::OrphanDocument);
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
            QStringLiteral("doc.large"), QStringLiteral("Large record"), pdf, false, {}}},
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
        });
    }

    RecordWorkspace workspace;
    QVERIFY(workspace.setRecord(std::move(definition)).has_value());
    workspace.setDocketFilter(QStringLiteral("search-token-9999"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    QVERIFY2(workspace.filterEvaluationCount() <= entry_count,
             "Filtering exceeded the deterministic one-row-inspection-per-entry budget");
}

} // namespace

QTEST_MAIN(RecordWorkspaceTest)

#include "tst_record_workspace.moc"
