#include "pack_cli.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

using appellate::cli::ExitCode;
using appellate::cli::runPackCli;
using appellate::cli::RunResult;

class PackCliTest final : public QObject {
    Q_OBJECT

  private slots:
    void completePackLifecycle();
    void rejectsInvalidArgumentsAndExistingTemplateDestination();
};

[[nodiscard]] QJsonObject responseObject(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

void requireSuccess(const RunResult& result, const QString& command) {
    QCOMPARE(result.exit_code, static_cast<int>(ExitCode::Success));
    QVERIFY2(result.standard_error.isEmpty(), result.standard_error.constData());
    QVERIFY(result.standard_output.endsWith('\n'));
    const auto response = responseObject(result.standard_output);
    QCOMPARE(response.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(response.value(QStringLiteral("command")).toString(), command);
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void PackCliTest::completePackLifecycle() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto pack_directory = QDir(temporary.path()).filePath(QStringLiteral("starter-pack"));
    const auto first_archive = QDir(temporary.path()).filePath(QStringLiteral("first.awpack"));
    const auto second_archive = QDir(temporary.path()).filePath(QStringLiteral("second.awpack"));
    const auto catalog_directory = QDir(temporary.path()).filePath(QStringLiteral("catalog"));

    const auto templated = runPackCli({QStringLiteral("template"), pack_directory});
    requireSuccess(templated, QStringLiteral("template"));
    const auto template_response = responseObject(templated.standard_output);
    QCOMPARE(template_response.value(QStringLiteral("resource_count")).toInt(), 12);
    QCOMPARE(template_response.value(QStringLiteral("blob_count")).toInt(), 1);
    QVERIFY(QFileInfo::exists(QDir(pack_directory).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(QFileInfo::exists(
        QDir(pack_directory).filePath(QStringLiteral("objects/final-order.pdf"))));

    const auto validated_directory = runPackCli({QStringLiteral("validate"), pack_directory});
    requireSuccess(validated_directory, QStringLiteral("validate"));
    QCOMPARE(responseObject(validated_directory.standard_output)
                 .value(QStringLiteral("source_kind"))
                 .toString(),
             QStringLiteral("directory"));
    QCOMPARE(responseObject(validated_directory.standard_output)
                 .value(QStringLiteral("blob_count"))
                 .toInt(),
             1);

    const auto first_export = runPackCli({QStringLiteral("export"), pack_directory, first_archive});
    requireSuccess(first_export, QStringLiteral("export"));
    const auto first_revision = responseObject(first_export.standard_output);

    const auto validated_archive = runPackCli({QStringLiteral("validate"), first_archive});
    requireSuccess(validated_archive, QStringLiteral("validate"));
    const auto archive_response = responseObject(validated_archive.standard_output);
    QCOMPARE(archive_response.value(QStringLiteral("source_kind")).toString(),
             QStringLiteral("archive"));
    QCOMPARE(archive_response.value(QStringLiteral("blob_count")).toInt(), 1);
    QCOMPARE(archive_response.value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));

    const auto second_export =
        runPackCli({QStringLiteral("export"), pack_directory, second_archive});
    requireSuccess(second_export, QStringLiteral("export"));
    const auto second_revision = responseObject(second_export.standard_output);
    QCOMPARE(second_revision.value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));
    QCOMPARE(readAll(first_archive), readAll(second_archive));

    const auto installed =
        runPackCli({QStringLiteral("install"), first_archive, catalog_directory,
                    QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T01:02:03Z")});
    requireSuccess(installed, QStringLiteral("install"));
    const auto install_response = responseObject(installed.standard_output);
    QCOMPARE(install_response.value(QStringLiteral("installed_at_utc")).toString(),
             QStringLiteral("2026-08-11T01:02:03Z"));

    const auto listed = runPackCli({QStringLiteral("list"), catalog_directory});
    requireSuccess(listed, QStringLiteral("list"));
    const auto packs =
        responseObject(listed.standard_output).value(QStringLiteral("packs")).toArray();
    QCOMPARE(packs.size(), 1);
    QCOMPARE(packs.at(0).toObject().value(QStringLiteral("digest")),
             first_revision.value(QStringLiteral("digest")));
    QCOMPARE(packs.at(0).toObject().value(QStringLiteral("pack_id")).toString(),
             QStringLiteral("example.full.fictional"));
}

void PackCliTest::rejectsInvalidArgumentsAndExistingTemplateDestination() {
    const auto missing = runPackCli({});
    QCOMPARE(missing.exit_code, static_cast<int>(ExitCode::InvalidArguments));
    QVERIFY(missing.standard_output.isEmpty());
    QCOMPARE(responseObject(missing.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("invalid_arguments"));

    const auto extra_validate =
        runPackCli({QStringLiteral("validate"), QStringLiteral("one"), QStringLiteral("two")});
    QCOMPARE(extra_validate.exit_code, static_cast<int>(ExitCode::InvalidArguments));

    const auto invalid_timestamp = runPackCli(
        {QStringLiteral("install"), QStringLiteral("pack.awpack"), QStringLiteral("catalog"),
         QStringLiteral("--installed-at"), QStringLiteral("2026-08-11T01:02:03+00:00")});
    QCOMPARE(invalid_timestamp.exit_code, static_cast<int>(ExitCode::InvalidArguments));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto existing = QDir(temporary.path()).filePath(QStringLiteral("existing"));
    QVERIFY(QDir{}.mkpath(existing));
    const auto marker_path = QDir(existing).filePath(QStringLiteral("keep.txt"));
    QFile marker(marker_path);
    QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(marker.write("preserve"), qint64{8});
    marker.close();

    const auto refused = runPackCli({QStringLiteral("template"), existing});
    QCOMPARE(refused.exit_code, static_cast<int>(ExitCode::OperationFailed));
    QCOMPARE(responseObject(refused.standard_error).value(QStringLiteral("code")).toString(),
             QStringLiteral("destination_exists"));
    QCOMPARE(readAll(marker_path), QByteArray("preserve"));
    QVERIFY(!QFileInfo::exists(QDir(existing).filePath(QStringLiteral("manifest.json"))));
}

} // namespace

QTEST_GUILESS_MAIN(PackCliTest)

#include "tst_pack_cli.moc"
