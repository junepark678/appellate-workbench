#include "record_workspace.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QPdfSearchModel>
#include <QTest>

#include <expected>
#include <utility>

#ifndef APPELLATE_M4_BLUEEMBER_ROOT
#error "APPELLATE_M4_BLUEEMBER_ROOT must name content/m4/blueember-jmol"
#endif

namespace {

namespace ui = appellate::ui;

constexpr auto kRecordSha256 = "080ff7772d73131a5471f2fc530b4d63c6215831a82ffcd671ef50beff8d1c7a";

[[nodiscard]] auto sha256(const QByteArray& bytes) -> QString {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] auto hasTag(const QJsonObject& entry, const QString& expected) -> bool {
    for (const auto& tag : entry.value(QStringLiteral("tags")).toArray()) {
        if (tag.toString() == expected) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto loadFrozenRecord() -> std::expected<ui::RecordDefinition, QString> {
    const QDir root(QStringLiteral(APPELLATE_M4_BLUEEMBER_ROOT));
    QFile file(root.filePath(QStringLiteral("pack-candidate/resources/record.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(file.errorString());
    }
    const auto bytes = file.readAll();
    if (sha256(bytes) != QString::fromLatin1(kRecordSha256)) {
        return std::unexpected(QStringLiteral("Frozen Blue Ember record digest drifted"));
    }

    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return std::unexpected(QStringLiteral("Frozen Blue Ember record is invalid JSON"));
    }
    const auto record = json.object();
    if (record.value(QStringLiteral("resource_id")).toString() !=
            QStringLiteral("ca4m4.blueember.record") ||
        record.value(QStringLiteral("docket_entries")).toArray().size() != 83 ||
        record.value(QStringLiteral("page_anchors")).toArray().size() != 656 ||
        record.value(QStringLiteral("dockets")).toArray().size() != 3) {
        return std::unexpected(QStringLiteral("Frozen Blue Ember record envelope drifted"));
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

    qsizetype lower_count = 0;
    qsizetype actual_count = 0;
    qsizetype counterfactual_count = 0;
    for (const auto& value : record.value(QStringLiteral("docket_entries")).toArray()) {
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("entry_id")).toString();
        const auto asset_path = object.value(QStringLiteral("asset_path")).toString();
        const auto local_path = root.filePath(QStringLiteral("pack-candidate/%1").arg(asset_path));
        definition.documents.emplace_back(
            id, object.value(QStringLiteral("title")).toString(), local_path,
            object.value(QStringLiteral("sealed")).toBool(true), QMap<QString, QString>{},
            object.value(QStringLiteral("page_count")).toInt());

        const auto is_lower = hasTag(object, QStringLiteral("lower_record"));
        const auto is_actual = hasTag(object, QStringLiteral("actual_appellate_docket"));
        const auto is_counterfactual =
            hasTag(object, QStringLiteral("counterfactual_appellate_branch"));
        const auto is_never_filed = hasTag(object, QStringLiteral("never_filed"));
        const auto never_actual = hasTag(object, QStringLiteral("never_occurred_on_actual_docket"));
        if (is_lower) {
            ++lower_count;
        }
        if (is_actual) {
            ++actual_count;
        }
        if (is_counterfactual) {
            ++counterfactual_count;
        }
        if ((is_counterfactual && (!is_never_filed || !never_actual || is_lower || is_actual)) ||
            (!is_counterfactual && (is_never_filed || never_actual))) {
            return std::unexpected(
                QStringLiteral("Actual/counterfactual docket isolation drifted"));
        }

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
    if (lower_count != 42 || actual_count != 16 || counterfactual_count != 25) {
        return std::unexpected(QStringLiteral("Blue Ember docket split drifted"));
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

class BlueEmberJmolUiE2eTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsAndFiltersActualAndNeverFiledDockets();
    void navigatesAndSearchesAcrossAllThreeDockets();
};

void BlueEmberJmolUiE2eTest::loadsAndFiltersActualAndNeverFiledDockets() {
    auto definition = loadFrozenRecord();
    QVERIFY2(definition.has_value(), definition ? "" : qPrintable(definition.error()));
    ui::RecordWorkspace workspace;
    const auto loaded = workspace.setRecord(std::move(*definition));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{83});

    workspace.setDocketFilter(QStringLiteral("lower_record"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{42});
    workspace.setDocketFilter(QStringLiteral("actual_appellate_docket"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{16});
    workspace.setDocketFilter(QStringLiteral("never_occurred_on_actual_docket"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{25});
    workspace.setDocketFilter(QStringLiteral("SYN-WDNC-24-CV-0520"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{42});
    workspace.setDocketFilter(QStringLiteral("SYN-CA4-26-CV-4105 actual_appellate_docket"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{16});
    workspace.setDocketFilter(QStringLiteral("SYN-CA4-26-CV-4105-CF-NEVER-OCCURRED"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{25});
    workspace.setDocketFilter(QStringLiteral("Granite Heron's Initial Rule 50(a) Motion"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
    workspace.setDocketFilter(QStringLiteral("Counterfactual Branch B01 Never Filed"));
    QCOMPARE(workspace.visibleDocketCount(), qsizetype{1});
}

void BlueEmberJmolUiE2eTest::navigatesAndSearchesAcrossAllThreeDockets() {
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
        QTRY_COMPARE_WITH_TIMEOUT(workspace.currentPageIndex(), page_index, 10'000);
        workspace.setDocumentSearch(query);
        QTRY_VERIFY_WITH_TIMEOUT(workspace.documentSearchResultCount() > 0, 10'000);
        workspace.setDocumentSearch(QString{});
    };

    check_navigation(QStringLiteral("JA373"), QStringLiteral("ca4m4.blueember.record.entry.l32"), 5,
                     0, QStringLiteral("causation only"));
    check_navigation(QStringLiteral("JA389"), QStringLiteral("ca4m4.blueember.record.entry.l36"), 9,
                     0, QStringLiteral("asserted expressly for the first time after the verdict"));
    check_navigation(QStringLiteral("PA87"), QStringLiteral("ca4m4.blueember.record.entry.a13"), 17,
                     0, QStringLiteral("reverse the mitigation JMOL"));
    check_navigation(QStringLiteral("PA109"), QStringLiteral("ca4m4.blueember.record.entry.b01"), 4,
                     0, QStringLiteral("one isolated premise"));
    check_navigation(QStringLiteral("PA162"), QStringLiteral("ca4m4.blueember.record.entry.b07"),
                     13, 0, QStringLiteral("twice made a precise pre-verdict"));
    check_navigation(QStringLiteral("PA220"), QStringLiteral("ca4m4.blueember.record.entry.b22"), 2,
                     0, QStringLiteral("clerk shall withhold mandate"));
    check_navigation(QStringLiteral("PA222"), QStringLiteral("ca4m4.blueember.record.entry.b23"), 2,
                     0, QStringLiteral("dissolved by this order"));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    BlueEmberJmolUiE2eTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_m4_blueember_jmol_ui_e2e.moc"
