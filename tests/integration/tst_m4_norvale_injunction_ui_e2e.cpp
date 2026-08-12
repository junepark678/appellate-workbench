#include "record_workspace.hpp"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPdfSearchModel>
#include <QTest>

#include <expected>
#include <utility>

#ifndef APPELLATE_M4_NORVALE_ROOT
#error "APPELLATE_M4_NORVALE_ROOT must name content/m4/norvale-injunction"
#endif

namespace {

namespace ui = appellate::ui;

[[nodiscard]] auto loadFrozenRecord() -> std::expected<ui::RecordDefinition, QString> {
    const QDir root(QStringLiteral(APPELLATE_M4_NORVALE_ROOT));
    QFile file(root.filePath(QStringLiteral("resources/record.candidate.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(file.errorString());
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return std::unexpected(QStringLiteral("Frozen Norvale record is invalid JSON"));
    }
    const auto record = json.object();
    if (record.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.norvale.record") ||
        record.value(QStringLiteral("docket_entries")).toArray().size() != 73 ||
        record.value(QStringLiteral("page_anchors")).toArray().size() != 383) {
        return std::unexpected(QStringLiteral("Frozen Norvale record envelope drifted"));
    }

    ui::RecordDefinition definition;
    QHash<QString, QString> docket_labels;
    for (const auto& value : record.value(QStringLiteral("dockets")).toArray()) {
        const auto object = value.toObject();
        ui::RecordDocketDescriptor docket;
        docket.id = object.value(QStringLiteral("docket_id")).toString();
        docket.type = object.value(QStringLiteral("docket_type")).toString();
        docket.court_id = object.value(QStringLiteral("court_id")).toString();
        docket.court_ref = object.value(QStringLiteral("court_ref")).toString();
        docket.public_docket_number =
            object.value(QStringLiteral("public_docket_number")).toString();
        docket.caption = object.value(QStringLiteral("caption")).toString();
        docket_labels.insert(docket.id, docket.public_docket_number);
        definition.dockets.push_back(std::move(docket));
    }

    for (const auto& value : record.value(QStringLiteral("docket_entries")).toArray()) {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("entry_id")).toString();
        const auto asset_path = object.value(QStringLiteral("asset_path")).toString();
        const auto local_path = root.filePath(QStringLiteral("pack-candidate/%1").arg(asset_path));
        definition.documents.emplace_back(
            id, object.value(QStringLiteral("title")).toString(), local_path,
            object.value(QStringLiteral("sealed")).toBool(true), QMap<QString, QString>{},
            object.value(QStringLiteral("page_count")).toInt());

        ui::RecordDocketEntry entry;
        entry.id = id;
        entry.filed_on =
            QDate::fromString(object.value(QStringLiteral("filed_on")).toString(), Qt::ISODate);
        entry.title = object.value(QStringLiteral("title")).toString();
        entry.actor = object.value(QStringLiteral("actor")).toString();
        entry.description = object.value(QStringLiteral("description")).toString();
        entry.document_id = id;
        for (const auto& tag : object.value(QStringLiteral("tags")).toArray()) {
            entry.tags.push_back(tag.toString());
        }
        entry.docket_id = object.value(QStringLiteral("docket_id")).toString();
        entry.docket_label = docket_labels.value(entry.docket_id);
        entry.entry_label = object.value(QStringLiteral("entry_label")).toString();
        definition.docket.push_back(std::move(entry));
    }

    QHash<QString, QString> document_by_entry;
    for (const auto& entry : definition.docket) {
        document_by_entry.insert(entry.id, entry.document_id);
    }
    for (const auto& value : record.value(QStringLiteral("page_anchors")).toArray()) {
        const auto object = value.toObject();
        ui::RecordPageAnchor anchor;
        anchor.id = object.value(QStringLiteral("anchor_id")).toString();
        anchor.document_id =
            document_by_entry.value(object.value(QStringLiteral("entry_id")).toString());
        anchor.page_index = object.value(QStringLiteral("page_number")).toInt() - 1;
        anchor.citation_label = object.value(QStringLiteral("citation_label")).toString();
        definition.anchors.push_back(std::move(anchor));
    }
    return definition;
}

class NorvaleInjunctionUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsAndFiltersAllThreeFrozenDockets();
    void navigatesAndSearchesDistrictActualAndCounterfactualPages();
};

void NorvaleInjunctionUiE2eTest::loadsAndFiltersAllThreeFrozenDockets() {
    auto definition = loadFrozenRecord();
    QVERIFY2(definition.has_value(), definition ? "" : qPrintable(definition.error()));
    ui::RecordWorkspace workspace;
    const auto loaded = workspace.setRecord(std::move(*definition));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{73});

    workspace.setDocketFilter(QStringLiteral("lower_record"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{24});
    workspace.setDocketFilter(QStringLiteral("actual_appellate_docket"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{23});
    workspace.setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{26});
    workspace.setDocketFilter(QStringLiteral("SYN-DSC-26-CV-0107"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{24});
    workspace.setDocketFilter(QStringLiteral("SYN-DSC-26-CV-0107 preliminary-injunction"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{4});
    workspace.setDocketFilter(QStringLiteral("Counterfactual Branch B09 Adverse Opinion"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
}

void NorvaleInjunctionUiE2eTest::navigatesAndSearchesDistrictActualAndCounterfactualPages() {
    auto definition = loadFrozenRecord();
    QVERIFY2(definition.has_value(), definition ? "" : qPrintable(definition.error()));
    ui::RecordWorkspace workspace;
    const auto loaded = workspace.setRecord(std::move(*definition));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));

    const auto check_navigation = [&](QString citation, QString entry_id, int page_count,
                                      int page_index, QString query) {
        const auto navigated = workspace.navigateToCitation(citation);
        QVERIFY2(navigated.has_value(), navigated ? "" : qPrintable(navigated.error().message));
        QCOMPARE(workspace.currentDocumentId(), entry_id);
        QCOMPARE(workspace.loadedPageCount(), page_count);
        QTRY_COMPARE(workspace.currentPageIndex(), page_index);
        workspace.setDocumentSearch(query);
        QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() > 0, 10'000);
        workspace.setDocumentSearch(QString{});
    };

    check_navigation(QStringLiteral("JA15"), QStringLiteral("ca4m4.norvale.record.entry.l03"), 8, 0,
                     QStringLiteral("Paid-Speaker Permit Ordinance"));
    check_navigation(QStringLiteral("JA93"), QStringLiteral("ca4m4.norvale.record.entry.l16"), 22,
                     0, QStringLiteral("Preliminary-Injunction Hearing Transcript"));
    check_navigation(QStringLiteral("PA44"), QStringLiteral("ca4m4.norvale.record.entry.a11"), 18,
                     0, QStringLiteral("content-neutral Paid-Speaker Permit Ordinance"));
    check_navigation(QStringLiteral("PA101"), QStringLiteral("ca4m4.norvale.record.entry.a16"), 5,
                     0, QStringLiteral("six-event series"));
    check_navigation(QStringLiteral("PA174"), QStringLiteral("ca4m4.norvale.record.entry.b09"), 16,
                     0, QStringLiteral("Adverse Opinion Reversing Preliminary Relief"));
    check_navigation(QStringLiteral("PA233"), QStringLiteral("ca4m4.norvale.record.entry.b26"), 2,
                     0, QStringLiteral("Adverse Mandate"));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    NorvaleInjunctionUiE2eTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_m4_norvale_injunction_ui_e2e.moc"
