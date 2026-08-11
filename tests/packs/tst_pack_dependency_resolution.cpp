#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"
#include "installed_record_controller.hpp"
#include "pack_cli.hpp"
#include "record_workspace.hpp"
#include "resolved_session_pins.hpp"
#include "workflow_session_controller.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <array>
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace {

using appellate::model::PackRevision;
using appellate::packs::CatalogErrorCode;
using appellate::packs::PackArchive;
using appellate::packs::PackCatalog;

class PackDependencyResolutionTest final : public QObject {
    Q_OBJECT

  private slots:
    void resolvesDiamondDependencyFirstWithSortedPins();
    void rejectsTransitiveVersionSplitWithoutPartialInstall();
    void rejectsGlobalResourceCollisionWithoutOverrides();
    void rejectsMissingTransitiveExactRevision();
    void detectsDependencyRowsThatDifferFromArchive();
    void capsClosureAt128Revisions();
    void scopesBlobMaterializationToResolvedClosure();
    void hydratesThinRootKeepsDependencyEntryPointsHiddenAndPinsSessions();
    void resolvesV2CanonicalAuthoritiesAcrossExactDependencies();
    void rejectsSiblingAssistedDependencyReference();
    void rejectsWrongExactDigestForBlobStreaming();
    void serializesPublicationAcrossCatalogInstances();
    void rollsBackNewArchiveAndBlobAfterFinalizationFailure();
};

[[nodiscard]] QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes.at(index) = static_cast<char>((value >> shift) & 0xffU);
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, const std::string& value) {
    addUint64(hash, value.size());
    hash.addData(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
}

[[nodiscard]] QString emptyBlobSetDigest(const PackRevision& revision) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, "appellate-workbench-catalog-blob-set-v1");
    addFrame(hash, revision.id.value);
    addFrame(hash, revision.version);
    addFrame(hash, revision.digest);
    addUint64(hash, 0);
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] bool writeAll(const QString& path, const QByteArray& bytes) {
    const QFileInfo info(path);
    if (!QDir{}.mkpath(info.path())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] QByteArray readAll(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

[[nodiscard]] QJsonObject dependency(const PackRevision& revision) {
    return QJsonObject{
        {QStringLiteral("pack_id"), QString::fromStdString(revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(revision.version)},
        {QStringLiteral("sha256"), QString::fromStdString(revision.digest)},
    };
}

[[nodiscard]] QJsonObject judgeProfile(const QString& resource_id, const QString& display_name) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("resource_kind"), QStringLiteral("judge_profile")},
        {QStringLiteral("resource_id"), resource_id},
        {QStringLiteral("display_name"), display_name},
        {QStringLiteral("profile_class"), QStringLiteral("fictional_composite")},
        {QStringLiteral("compatibility"),
         QJsonObject{
             {QStringLiteral("court_roles"), QJsonArray{QStringLiteral("appellate")}},
             {QStringLiteral("jurisdiction_ids"), QJsonArray{QStringLiteral("us.ca4")}},
         }},
        {QStringLiteral("interaction"),
         QJsonObject{
             {QStringLiteral("directness"), 0.5},
             {QStringLiteral("formality"), 0.5},
             {QStringLiteral("question_length"), 0.5},
             {QStringLiteral("interruption_frequency"), 0.2},
             {QStringLiteral("follow_up_depth"), 0.5},
             {QStringLiteral("hypothetical_frequency"), 0.4},
             {QStringLiteral("concession_recall"), 0.6},
             {QStringLiteral("record_pin_demand"), 0.7},
             {QStringLiteral("time_strictness"), 0.5},
             {QStringLiteral("issue_focus"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("topic_id"), QStringLiteral("test.issue.preservation")},
                  {QStringLiteral("weight"), 0.8},
              }}},
         }},
        {QStringLiteral("voice"),
         QJsonObject{
             {QStringLiteral("register"), QStringLiteral("formal")},
             {QStringLiteral("cadence"), QStringLiteral("measured")},
             {QStringLiteral("question_framing"), QStringLiteral("direct")},
             {QStringLiteral("address_convention"), QStringLiteral("counsel")},
             {QStringLiteral("verbosity"), 0.5},
             {QStringLiteral("sentence_complexity"), 0.5},
             {QStringLiteral("question_phrases"),
              QJsonArray{QStringLiteral("address the question")}},
             {QStringLiteral("interruption_phrases"), QJsonArray{QStringLiteral("pause there")}},
             {QStringLiteral("clarification_phrases"),
              QJsonArray{QStringLiteral("clarify that point")}},
         }},
    };
}

[[nodiscard]] auto buildArchive(const QString& root, const QString& stem, const QString& pack_id,
                                const QString& version,
                                const std::vector<PackRevision>& dependencies = {},
                                const QString& forced_resource_id = {})
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto archive =
        QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
    const auto resource_id = forced_resource_id.isEmpty()
                                 ? pack_id + QStringLiteral(".judge.measured")
                                 : forced_resource_id;
    const auto profile =
        QJsonDocument(judgeProfile(resource_id, pack_id)).toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(QStringLiteral("judges/measured.json")), profile) ||
        !QDir{}.mkpath(QFileInfo(archive).path())) {
        return std::unexpected(QStringLiteral("cannot write pack payload"));
    }
    QJsonArray dependency_values;
    for (const auto& required : dependencies) {
        dependency_values.push_back(dependency(required));
    }
    const auto manifest =
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("pack_id"), pack_id},
                {QStringLiteral("version"), version},
                {QStringLiteral("required_capabilities"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("id"), QStringLiteral("workbench.pack.judge-profile")},
                     {QStringLiteral("version"), 1},
                 }}},
                {QStringLiteral("dependencies"), dependency_values},
                {QStringLiteral("blobs"), QJsonArray{}},
                {QStringLiteral("contents"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("id"), resource_id},
                     {QStringLiteral("kind"), QStringLiteral("judge_profile")},
                     {QStringLiteral("schema_version"), 1},
                     {QStringLiteral("path"), QStringLiteral("judges/measured.json")},
                     {QStringLiteral("sha256"), QString::fromLatin1(sha256(profile))},
                 }}},
            })
            .toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(QStringLiteral("manifest.json")), manifest)) {
        return std::unexpected(QStringLiteral("cannot write manifest"));
    }
    const auto exported = PackArchive::exportDirectory(source, archive);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return *exported;
}

[[nodiscard]] auto buildBlobArchive(const QString& root, const QString& stem,
                                    const QString& pack_id, QByteArray label,
                                    const std::vector<PackRevision>& dependencies = {})
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto archive =
        QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
    const auto resource_id = pack_id + QStringLiteral(".record.main");
    const auto asset_path = QStringLiteral("objects/document.pdf");
    const auto pdf = QByteArray("%PDF-1.7\n% ") + label +
                     QByteArray("\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n");
    const auto pdf_digest = sha256(pdf);
    const auto record =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("schema_version"), 1},
                          {QStringLiteral("resource_kind"), QStringLiteral("record")},
                          {QStringLiteral("resource_id"), resource_id},
                          {QStringLiteral("caption"), pack_id},
                          {QStringLiteral("docket_entries"),
                           QJsonArray{QJsonObject{
                               {QStringLiteral("entry_id"), pack_id + QStringLiteral(".entry.one")},
                               {QStringLiteral("entry_number"), 1},
                               {QStringLiteral("filed_on"), QStringLiteral("2026-08-11")},
                               {QStringLiteral("title"), QStringLiteral("Synthetic document")},
                               {QStringLiteral("asset_path"), asset_path},
                               {QStringLiteral("asset_sha256"), QString::fromLatin1(pdf_digest)},
                               {QStringLiteral("page_count"), 1},
                               {QStringLiteral("sealed"), false},
                           }}},
                      })
            .toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(asset_path), pdf) ||
        !writeAll(QDir(source).filePath(QStringLiteral("resources/record.json")), record) ||
        !QDir{}.mkpath(QFileInfo(archive).path())) {
        return std::unexpected(QStringLiteral("cannot write blob pack payload"));
    }
    QJsonArray dependency_values;
    for (const auto& required : dependencies) {
        dependency_values.push_back(dependency(required));
    }
    const auto manifest =
        QJsonDocument(
            QJsonObject{
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("pack_id"), pack_id},
                {QStringLiteral("version"), QStringLiteral("1.0.0")},
                {QStringLiteral("required_capabilities"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                     {QStringLiteral("version"), 1},
                 }}},
                {QStringLiteral("dependencies"), dependency_values},
                {QStringLiteral("blobs"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("path"), asset_path},
                     {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
                     {QStringLiteral("byte_size"), pdf.size()},
                     {QStringLiteral("sha256"), QString::fromLatin1(pdf_digest)},
                 }}},
                {QStringLiteral("contents"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("id"), resource_id},
                     {QStringLiteral("kind"), QStringLiteral("record")},
                     {QStringLiteral("schema_version"), 1},
                     {QStringLiteral("path"), QStringLiteral("resources/record.json")},
                     {QStringLiteral("sha256"), QString::fromLatin1(sha256(record))},
                 }}},
            })
            .toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(QStringLiteral("manifest.json")), manifest)) {
        return std::unexpected(QStringLiteral("cannot write blob pack manifest"));
    }
    const auto exported = PackArchive::exportDirectory(source, archive);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return *exported;
}

[[nodiscard]] auto
buildPartitionArchive(const QString& root, const QString& stem, const QString& pack_id,
                      const std::vector<QString>& resource_paths,
                      const std::vector<PackRevision>& dependencies = {}, bool include_blob = false,
                      const QByteArray& replacement_prefix = {}, int schema_version = 1)
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto archive =
        QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
    QJsonArray contents;
    const auto fixture_root = QStringLiteral(APPELLATE_TEST_FIXTURES) +
                              (schema_version == 2 ? QStringLiteral("/full-resource-pack-v2")
                                                   : QStringLiteral("/full-resource-pack"));
    const auto fixture_manifest =
        QJsonDocument::fromJson(
            readAll(QDir(fixture_root).filePath(QStringLiteral("manifest.json"))))
            .object();
    for (const auto& resource_path : resource_paths) {
        auto payload = readAll(QDir(fixture_root).filePath(resource_path));
        if (!replacement_prefix.isEmpty()) {
            payload.replace(QByteArray("example."), replacement_prefix);
        }
        const auto document = QJsonDocument::fromJson(payload);
        if (payload.isEmpty() || !document.isObject() ||
            !writeAll(QDir(source).filePath(resource_path), payload)) {
            return std::unexpected(QStringLiteral("cannot partition fixture resource"));
        }
        const auto object = document.object();
        contents.push_back(QJsonObject{
            {QStringLiteral("id"), object.value(QStringLiteral("resource_id"))},
            {QStringLiteral("kind"), object.value(QStringLiteral("resource_kind"))},
            {QStringLiteral("schema_version"), object.value(QStringLiteral("schema_version"))},
            {QStringLiteral("path"), resource_path},
            {QStringLiteral("sha256"), QString::fromLatin1(sha256(payload))},
        });
    }
    QJsonArray blobs;
    if (include_blob) {
        blobs = fixture_manifest.value(QStringLiteral("blobs")).toArray();
        for (const auto& value : blobs) {
            const auto path = value.toObject().value(QStringLiteral("path")).toString();
            const auto payload = readAll(QDir(fixture_root).filePath(path));
            if (payload.isEmpty() || !writeAll(QDir(source).filePath(path), payload)) {
                return std::unexpected(QStringLiteral("cannot partition fixture blob"));
            }
        }
    }
    QJsonArray dependency_values;
    for (const auto& required : dependencies) {
        dependency_values.push_back(dependency(required));
    }
    const auto manifest =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("schema_version"), schema_version},
                          {QStringLiteral("pack_id"), pack_id},
                          {QStringLiteral("version"), QStringLiteral("1.0.0")},
                          {QStringLiteral("required_capabilities"),
                           fixture_manifest.value(QStringLiteral("required_capabilities"))},
                          {QStringLiteral("dependencies"), dependency_values},
                          {QStringLiteral("blobs"), blobs},
                          {QStringLiteral("contents"), contents},
                      })
            .toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(QStringLiteral("manifest.json")), manifest) ||
        !QDir{}.mkpath(QFileInfo(archive).path())) {
        return std::unexpected(QStringLiteral("cannot write partition manifest"));
    }
    const auto exported = PackArchive::exportDirectory(
        source, archive, {}, appellate::packs::PackValidationScope::ResolvedClosure);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return *exported;
}

[[nodiscard]] QString archivePath(const QString& root, const QString& stem) {
    return QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
}

[[nodiscard]] bool
seedCatalogWithoutResolution(const QString& catalog_root, const QString& source_root,
                             const std::vector<std::pair<PackRevision, QString>>& revisions) {
    const auto connection_name =
        QStringLiteral("dependency-seed-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = true;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite")));
        succeeded = database.open();
        QSqlQuery foreign_keys(database);
        succeeded = succeeded && foreign_keys.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        QSqlQuery transaction(database);
        succeeded = succeeded && transaction.exec(QStringLiteral("BEGIN IMMEDIATE"));
        for (const auto& [revision, stem] : revisions) {
            if (!succeeded) {
                break;
            }
            const auto source = archivePath(source_root, stem);
            const auto archive_bytes = readAll(source);
            const auto archive_sha = QString::fromLatin1(sha256(archive_bytes));
            const auto destination = QDir(catalog_root)
                                         .filePath(QStringLiteral("archives/") + archive_sha +
                                                   QStringLiteral(".awpack"));
            if (archive_bytes.isEmpty() ||
                (!QFileInfo::exists(destination) && !QFile::copy(source, destination))) {
                succeeded = false;
                break;
            }
            const auto loaded = PackArchive::importArchive(source);
            if (!loaded || loaded->revision != revision || !loaded->blobs.empty()) {
                succeeded = false;
                break;
            }
            QSqlQuery pack(database);
            pack.prepare(QStringLiteral(
                "INSERT INTO pack_revisions(pack_id, version, digest, archive_sha256, "
                "installed_at_utc) VALUES(?, ?, ?, ?, ?)"));
            pack.addBindValue(QString::fromStdString(revision.id.value));
            pack.addBindValue(QString::fromStdString(revision.version));
            pack.addBindValue(QString::fromStdString(revision.digest));
            pack.addBindValue(archive_sha);
            pack.addBindValue(QStringLiteral("2026-08-11T00:00:00Z"));
            succeeded = pack.exec();
            QSqlQuery blob_set(database);
            blob_set.prepare(QStringLiteral(
                "INSERT INTO pack_blob_sets(pack_id, version, blob_count, descriptor_sha256) "
                "VALUES(?, ?, 0, ?)"));
            blob_set.addBindValue(QString::fromStdString(revision.id.value));
            blob_set.addBindValue(QString::fromStdString(revision.version));
            blob_set.addBindValue(emptyBlobSetDigest(revision));
            succeeded = succeeded && blob_set.exec();
            for (const auto& required : loaded->dependencies) {
                QSqlQuery dependency_insert(database);
                dependency_insert.prepare(QStringLiteral(
                    "INSERT INTO pack_dependencies(pack_id, version, dependency_pack_id, "
                    "dependency_version, dependency_digest) VALUES(?, ?, ?, ?, ?)"));
                dependency_insert.addBindValue(QString::fromStdString(revision.id.value));
                dependency_insert.addBindValue(QString::fromStdString(revision.version));
                dependency_insert.addBindValue(QString::fromStdString(required.revision.id.value));
                dependency_insert.addBindValue(QString::fromStdString(required.revision.version));
                dependency_insert.addBindValue(QString::fromStdString(required.revision.digest));
                succeeded = succeeded && dependency_insert.exec();
            }
        }
        QSqlQuery finish(database);
        succeeded = succeeded && finish.exec(QStringLiteral("COMMIT"));
        if (!succeeded) {
            QSqlQuery rollback(database);
            static_cast<void>(rollback.exec(QStringLiteral("ROLLBACK")));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] bool install(PackCatalog& catalog, const QString& root, const QString& stem,
                           int second) {
    return catalog
        .installArchive(archivePath(root, stem),
                        QStringLiteral("2026-08-11T00:00:%1Z").arg(second, 2, 10, QLatin1Char('0')))
        .has_value();
}

[[nodiscard]] bool executeCatalogSql(const QString& catalog_root, const QString& statement) {
    const auto connection_name =
        QStringLiteral("dependency-resolution-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(QDir(catalog_root).filePath(QStringLiteral("catalog.sqlite")));
        succeeded = database.open();
        if (succeeded) {
            QSqlQuery query(database);
            succeeded = query.exec(statement);
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] std::optional<QByteArray>
sessionEventPayload(const QString& database_path, const QString& session_id, qint64 sequence) {
    const auto connection_name =
        QStringLiteral("session-event-read-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    std::optional<QByteArray> payload;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral("SELECT payload_json FROM event_log "
                                         "WHERE session_id = ? AND sequence = ?"));
            query.addBindValue(session_id);
            query.addBindValue(sequence);
            if (query.exec() && query.next()) {
                payload = query.value(0).toByteArray();
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return payload;
}

[[nodiscard]] bool writeSessionEventPayload(const QString& database_path, const QString& session_id,
                                            qint64 sequence, const QByteArray& payload) {
    const auto connection_name =
        QStringLiteral("session-event-write-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name);
        database.setDatabaseName(database_path);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "UPDATE event_log SET payload_json = ? WHERE session_id = ? AND sequence = ?"));
            query.addBindValue(payload);
            query.addBindValue(session_id);
            query.addBindValue(sequence);
            succeeded = query.exec() && query.numRowsAffected() == 1;
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection_name);
    return succeeded;
}

[[nodiscard]] QByteArray
mutateAuthorityProvenance(const QByteArray& encoded,
                          const std::function<void(QJsonObject&)>& mutate_provenance) {
    auto envelope = QJsonDocument::fromJson(encoded).object();
    auto payload = envelope.value(QStringLiteral("payload")).toObject();
    auto basis = payload.value(QStringLiteral("authority")).toObject();
    auto primary = basis.value(QStringLiteral("primary")).toObject();
    auto provenance = primary.value(QStringLiteral("provenance")).toObject();
    mutate_provenance(provenance);
    primary.insert(QStringLiteral("provenance"), provenance);
    basis.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), basis);
    envelope.insert(QStringLiteral("payload"), payload);
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

void PackDependencyResolutionTest::resolvesDiamondDependencyFirstWithSortedPins() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto leaf = buildArchive(temporary.path(), QStringLiteral("leaf"),
                                   QStringLiteral("test.dep.leaf"), QStringLiteral("1.0.0"));
    QVERIFY(leaf.has_value());
    const auto left =
        buildArchive(temporary.path(), QStringLiteral("left"), QStringLiteral("test.dep.left"),
                     QStringLiteral("1.0.0"), {*leaf});
    const auto right =
        buildArchive(temporary.path(), QStringLiteral("right"), QStringLiteral("test.dep.right"),
                     QStringLiteral("1.0.0"), {*leaf});
    QVERIFY(left.has_value());
    QVERIFY(right.has_value());
    const auto root =
        buildArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.dep.root"),
                     QStringLiteral("1.0.0"), {*right, *left});
    QVERIFY(root.has_value());

    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("leaf"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("left"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("right"), 3));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("root"), 4));

    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY(resolved.has_value());
    QCOMPARE(resolved->root().revision, *root);
    const auto dependencies = resolved->dependenciesDependencyFirst();
    QCOMPARE(dependencies.size(), std::size_t{3});
    QCOMPARE(dependencies[0].revision, *leaf);
    QCOMPARE(dependencies[1].revision, *left);
    QCOMPARE(dependencies[2].revision, *right);

    const auto pins = resolved->revisionsByPackId();
    QCOMPARE(pins.size(), std::size_t{4});
    QCOMPARE(pins[0].id.value, std::string("test.dep.leaf"));
    QCOMPARE(pins[1].id.value, std::string("test.dep.left"));
    QCOMPARE(pins[2].id.value, std::string("test.dep.right"));
    QCOMPARE(pins[3].id.value, std::string("test.dep.root"));
    QVERIFY(resolved->resourceOwner("test.dep.leaf.judge.measured") ==
            std::optional<PackRevision>(*leaf));
}

void PackDependencyResolutionTest::rejectsTransitiveVersionSplitWithoutPartialInstall() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto common_v1 = buildArchive(temporary.path(), QStringLiteral("common-v1"),
                                        QStringLiteral("test.dep.common"), QStringLiteral("1.0.0"));
    const auto common_v2 = buildArchive(temporary.path(), QStringLiteral("common-v2"),
                                        QStringLiteral("test.dep.common"), QStringLiteral("2.0.0"));
    QVERIFY(common_v1.has_value());
    QVERIFY(common_v2.has_value());
    const auto left =
        buildArchive(temporary.path(), QStringLiteral("left"), QStringLiteral("test.dep.left"),
                     QStringLiteral("1.0.0"), {*common_v1});
    const auto right =
        buildArchive(temporary.path(), QStringLiteral("right"), QStringLiteral("test.dep.right"),
                     QStringLiteral("1.0.0"), {*common_v2});
    QVERIFY(left.has_value());
    QVERIFY(right.has_value());
    const auto root =
        buildArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.dep.root"),
                     QStringLiteral("1.0.0"), {*left, *right});
    QVERIFY(root.has_value());

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("common-v1"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("common-v2"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("left"), 3));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("right"), 4));
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("root")),
                                   QStringLiteral("2026-08-11T00:00:05Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::DependencyVersionSplit);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{4});
    QCOMPARE(QDir((*catalog)->archivesDirectory())
                 .entryList(QStringList{QStringLiteral("*.awpack")}, QDir::Files)
                 .size(),
             4);
}

void PackDependencyResolutionTest::rejectsGlobalResourceCollisionWithoutOverrides() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto shared_id = QStringLiteral("test.dep.shared.judge");
    const auto left =
        buildArchive(temporary.path(), QStringLiteral("left"), QStringLiteral("test.dep.left"),
                     QStringLiteral("1.0.0"), {}, shared_id);
    const auto right =
        buildArchive(temporary.path(), QStringLiteral("right"), QStringLiteral("test.dep.right"),
                     QStringLiteral("1.0.0"), {}, shared_id);
    QVERIFY(left.has_value());
    QVERIFY(right.has_value());
    const auto root =
        buildArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.dep.root"),
                     QStringLiteral("1.0.0"), {*left, *right});
    QVERIFY(root.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("left"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("right"), 2));
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("root")),
                                   QStringLiteral("2026-08-11T00:00:03Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::ResourceCollision);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{2});
    QCOMPARE(QDir((*catalog)->archivesDirectory())
                 .entryList(QStringList{QStringLiteral("*.awpack")}, QDir::Files)
                 .size(),
             2);
}

void PackDependencyResolutionTest::rejectsMissingTransitiveExactRevision() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto leaf = buildArchive(temporary.path(), QStringLiteral("leaf"),
                                   QStringLiteral("test.missing.leaf"), QStringLiteral("1.0.0"));
    QVERIFY(leaf.has_value());
    const auto middle =
        buildArchive(temporary.path(), QStringLiteral("middle"),
                     QStringLiteral("test.missing.middle"), QStringLiteral("1.0.0"), {*leaf});
    QVERIFY(middle.has_value());
    const auto root =
        buildArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.missing.root"),
                     QStringLiteral("1.0.0"), {*middle});
    QVERIFY(root.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("leaf"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("middle"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("root"), 3));

    // This simulates catalog loss from an older/crashed installation. Direct rows in the
    // verified middle archive still pin the leaf, so resolution must not silently truncate.
    QVERIFY(executeCatalogSql(
        catalog_root,
        QStringLiteral("DELETE FROM pack_revisions WHERE pack_id = 'test.missing.leaf'")));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY(!resolved.has_value());
    QCOMPARE(resolved.error().code, CatalogErrorCode::MissingDependency);
}

void PackDependencyResolutionTest::detectsDependencyRowsThatDifferFromArchive() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto base = buildArchive(temporary.path(), QStringLiteral("base"),
                                   QStringLiteral("test.dep.base"), QStringLiteral("1.0.0"));
    QVERIFY(base.has_value());
    const auto root =
        buildArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.dep.root"),
                     QStringLiteral("1.0.0"), {*base});
    QVERIFY(root.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("base"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("root"), 2));

    QVERIFY(executeCatalogSql(
        catalog_root, QStringLiteral("UPDATE pack_dependencies SET dependency_digest = '%1' "
                                     "WHERE pack_id = 'test.dep.root'")
                          .arg(QString(64, u'a'))));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY(!resolved.has_value());
    QCOMPARE(resolved.error().code, CatalogErrorCode::CorruptCatalog);
}

void PackDependencyResolutionTest::capsClosureAt128Revisions() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    std::vector<std::pair<PackRevision, QString>> revisions;
    revisions.reserve(129);
    std::optional<PackRevision> previous;
    for (int index = 1; index <= 129; ++index) {
        const auto stem = QStringLiteral("limit-%1").arg(index, 3, 10, QLatin1Char('0'));
        const auto pack_id = QStringLiteral("test.limit.p%1").arg(index, 3, 10, QLatin1Char('0'));
        const std::vector<PackRevision> dependencies = previous.has_value()
                                                           ? std::vector<PackRevision>{*previous}
                                                           : std::vector<PackRevision>{};
        const auto revision =
            buildArchive(temporary.path(), stem, pack_id, QStringLiteral("1.0.0"), dependencies);
        QVERIFY(revision.has_value());
        revisions.emplace_back(*revision, stem);
        previous = *revision;
    }
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto initialized = PackCatalog::open(catalog_root);
        QVERIFY(initialized.has_value());
    }
    QVERIFY(seedCatalogWithoutResolution(catalog_root, temporary.path(), revisions));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    const auto accepted = (*catalog)->loadResolved(revisions.at(127).first);
    QVERIFY(accepted.has_value());
    QCOMPARE(accepted->revisionsByPackId().size(), std::size_t{128});
    const auto rejected = (*catalog)->loadResolved(revisions.at(128).first);
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::DependencyClosureTooLarge);
}

void PackDependencyResolutionTest::scopesBlobMaterializationToResolvedClosure() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto dependency_revision =
        buildBlobArchive(temporary.path(), QStringLiteral("dependency"),
                         QStringLiteral("test.dep.library"), QByteArray("dependency bytes"));
    QVERIFY(dependency_revision.has_value());
    const auto root =
        buildBlobArchive(temporary.path(), QStringLiteral("root"), QStringLiteral("test.dep.root"),
                         QByteArray("root bytes"), {*dependency_revision});
    const auto outside = buildArchive(temporary.path(), QStringLiteral("outside"),
                                      QStringLiteral("test.dep.outside"), QStringLiteral("1.0.0"));
    QVERIFY(root.has_value());
    QVERIFY(outside.has_value());
    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("dependency"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("root"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("outside"), 3));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY(resolved.has_value());
    const auto root_blob = (*catalog)->materializeBlob(*resolved, *root, "objects/document.pdf");
    const auto dependency_blob =
        (*catalog)->materializeBlob(*resolved, *dependency_revision, "objects/document.pdf");
    QVERIFY(root_blob.has_value());
    QVERIFY(dependency_blob.has_value());
    QVERIFY(root_blob->descriptor.sha256 != dependency_blob->descriptor.sha256);
    QVERIFY(root_blob->local_path != dependency_blob->local_path);
    QFile root_file(root_blob->local_path);
    QFile dependency_file(dependency_blob->local_path);
    QVERIFY(root_file.open(QIODevice::ReadOnly));
    QVERIFY(dependency_file.open(QIODevice::ReadOnly));
    QVERIFY(root_file.readAll().contains("root bytes"));
    QVERIFY(dependency_file.readAll().contains("dependency bytes"));

    const auto materialized =
        (*catalog)->materializeBlob(*resolved, *outside, "objects/not-owned.pdf");
    QVERIFY(!materialized.has_value());
    QCOMPARE(materialized.error().code, CatalogErrorCode::InvalidConfiguration);
}

void PackDependencyResolutionTest::
    hydratesThinRootKeepsDependencyEntryPointsHiddenAndPinsSessions() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto leaf = buildPartitionArchive(
        temporary.path(), QStringLiteral("leaf"), QStringLiteral("test.thin.leaf"),
        {QStringLiteral("resources/authority-set.json"), QStringLiteral("resources/court.json"),
         QStringLiteral("resources/judge-profile.json")});
    QVERIFY(leaf.has_value());
    const auto left = buildPartitionArchive(
        temporary.path(), QStringLiteral("left"), QStringLiteral("test.thin.left"),
        {QStringLiteral("resources/filing-catalog.json"), QStringLiteral("resources/form.json"),
         QStringLiteral("resources/procedure-profile.json"),
         QStringLiteral("resources/workflow.json")},
        {*leaf});
    const auto right = buildPartitionArchive(
        temporary.path(), QStringLiteral("right"), QStringLiteral("test.thin.right"),
        {QStringLiteral("resources/bench-configuration.json")}, {*leaf});
    QVERIFY(left.has_value());
    QVERIFY(right.has_value());

    const std::vector<QString> all_resource_paths{
        QStringLiteral("resources/argument-config.json"),
        QStringLiteral("resources/authority-set.json"),
        QStringLiteral("resources/bench-configuration.json"),
        QStringLiteral("resources/case.json"),
        QStringLiteral("resources/court.json"),
        QStringLiteral("resources/filing-catalog.json"),
        QStringLiteral("resources/form.json"),
        QStringLiteral("resources/judge-profile.json"),
        QStringLiteral("resources/procedure-profile.json"),
        QStringLiteral("resources/realism-review.json"),
        QStringLiteral("resources/record.json"),
        QStringLiteral("resources/workflow.json"),
    };
    const auto hidden_library =
        buildPartitionArchive(temporary.path(), QStringLiteral("hidden-library"),
                              QStringLiteral("test.thin.hidden-library"), all_resource_paths, {},
                              true, QByteArray("library."));
    QVERIFY(hidden_library.has_value());
    const auto root = buildPartitionArchive(
        temporary.path(), QStringLiteral("root"), QStringLiteral("test.thin.root"),
        {QStringLiteral("resources/argument-config.json"), QStringLiteral("resources/case.json"),
         QStringLiteral("resources/realism-review.json"), QStringLiteral("resources/record.json")},
        {*left, *right, *hidden_library}, true);
    if (!root) {
        QFAIL(qPrintable(root.error()));
    }

    const auto root_source = QDir(temporary.path()).filePath(QStringLiteral("sources/root"));
    const auto cli_strict_archive =
        QDir(temporary.path()).filePath(QStringLiteral("cli-strict.awpack"));
    const auto cli_deferred_archive =
        QDir(temporary.path()).filePath(QStringLiteral("cli-deferred.awpack"));
    const auto strict_cli =
        appellate::cli::runPackCli({QStringLiteral("export"), root_source, cli_strict_archive});
    QCOMPARE(strict_cli.exit_code, static_cast<int>(appellate::cli::ExitCode::InvalidPack));
    QVERIFY(!QFileInfo::exists(cli_strict_archive));
    const auto deferred_cli = appellate::cli::runPackCli(
        {QStringLiteral("export-deferred"), root_source, cli_deferred_archive});
    QCOMPARE(deferred_cli.exit_code, static_cast<int>(appellate::cli::ExitCode::Success));
    const auto deferred_response = QJsonDocument::fromJson(deferred_cli.standard_output).object();
    QCOMPARE(deferred_response.value(QStringLiteral("validation_scope")).toString(),
             QStringLiteral("deferred_references"));
    QCOMPARE(deferred_response.value(QStringLiteral("resolved")).toBool(), false);
    const auto standalone = appellate::packs::PackReader::readDirectory(root_source);
    QVERIFY(!standalone.has_value());
    QCOMPARE(standalone.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
    const auto deferred = appellate::packs::PackReader::readDirectory(
        root_source, appellate::packs::PackValidationScope::ResolvedClosure);
    QVERIFY(deferred.has_value());
    QCOMPARE(deferred->graph_state, appellate::packs::PackGraphState::DeferredReferences);
    const auto bypass = appellate::packs::loadRuntimePack(*deferred);
    QVERIFY(!bypass.has_value());
    QCOMPARE(bypass.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("leaf"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("left"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("right"), 3));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("hidden-library"), 4));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("root"), 5));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY(resolved.has_value());
    const auto cli_resolved = appellate::cli::runPackCli(
        {QStringLiteral("validate-resolved"),
         QDir(temporary.path()).filePath(QStringLiteral("catalog")),
         QString::fromStdString(root->id.value), QString::fromStdString(root->version),
         QString::fromStdString(root->digest)});
    QCOMPARE(cli_resolved.exit_code, static_cast<int>(appellate::cli::ExitCode::Success));
    const auto resolved_response = QJsonDocument::fromJson(cli_resolved.standard_output).object();
    QCOMPARE(resolved_response.value(QStringLiteral("validation_scope")).toString(),
             QStringLiteral("catalog_resolved"));
    QCOMPARE(resolved_response.value(QStringLiteral("revision_pins")).toArray().size(), 5);
    const auto runtime = appellate::packs::loadRuntimePack(*resolved);
    if (!runtime) {
        QFAIL(runtime.error().message.c_str());
    }
    QCOMPARE(runtime->revision, *root);
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    QCOMPARE(runtime->cases.front().definition.id.value, std::string("example.case.fictional"));
    QCOMPARE(runtime->cases.front().record.id.value, std::string("example.record.fictional"));

    // The installed path must rebuild and compare the resolved closure, then load only the
    // root-owned record/blob from catalog storage. Authoring sources are deliberately gone.
    QVERIFY(QDir(QDir(temporary.path()).filePath(QStringLiteral("sources"))).removeRecursively());
    QVERIFY(QDir(QDir(temporary.path()).filePath(QStringLiteral("archives"))).removeRecursively());
    QDir installed_archives((*catalog)->archivesDirectory());
    for (const auto& archive : installed_archives.entryList(QDir::Files)) {
        QVERIFY(installed_archives.remove(archive));
    }
    QVERIFY(installed_archives.entryList(QDir::Files).empty());
    appellate::ui::RecordWorkspace workspace;
    appellate::app::InstalledRecordController record_controller(**catalog, workspace);
    const auto installed_record =
        record_controller.load(*resolved, *runtime, runtime->cases.front().definition.id);
    if (!installed_record) {
        QFAIL(qPrintable(installed_record.error().message));
    }
    QCOMPARE(installed_record->revision, *root);
    QCOMPARE(installed_record->assets.size(), std::size_t{1});
    QCOMPARE(installed_record->assets.front().descriptor, resolved->root().blobs.front());
    QVERIFY(QFileInfo::exists(installed_record->assets.front().local_path));
    auto noncanonical_runtime = *runtime;
    noncanonical_runtime.cases.front().title += " (mutated)";
    const auto refused_runtime = record_controller.load(*resolved, noncanonical_runtime,
                                                        runtime->cases.front().definition.id);
    QVERIFY(!refused_runtime.has_value());
    QCOMPARE(refused_runtime.error().code,
             appellate::app::InstalledRecordErrorCode::RuntimeMismatch);

    const auto pins = appellate::app::revisionPinsForSession(*resolved);
    QCOMPARE(pins.size(), resolved->revisionsByPackId().size());
    for (std::size_t index = 0; index < pins.size(); ++index) {
        QCOMPARE(pins[index].pack_id,
                 QString::fromStdString(resolved->revisionsByPackId()[index].id.value));
    }
    const auto& runtime_case = runtime->cases.front();
    const auto initial_state = appellate::model::WorkflowState{
        "test.session.resolved-closure",
        runtime_case.workflow.id,
        runtime_case.workflow.initial_stage_id,
        1,
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
    QTemporaryDir session_directory;
    QVERIFY(session_directory.isValid());
    const auto session_database =
        QDir(session_directory.path()).filePath(QStringLiteral("sessions.sqlite"));
    const auto session_assets = QDir(session_directory.path()).filePath(QStringLiteral("assets"));
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        if (!store) {
            QFAIL(qPrintable(store.error().message));
        }
        auto created = appellate::app::WorkflowSessionController::create(
            runtime_case.workflow, runtime_case.definition, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QStringLiteral("engine.resolved-closure.test.1"),
            QStringLiteral("2026-08-11T12:00:00Z"), *resolved);
        if (!created) {
            QFAIL(qPrintable(created.error().message));
        }
        QVERIFY((*created)->snapshot().pins == pins);
    }

    const auto reopen_with = [&](std::vector<appellate::storage::RevisionPin> expected_pins) {
        using Controller = appellate::app::WorkflowSessionController;
        using Error = appellate::app::WorkflowSessionError;
        auto store = appellate::storage::SessionStore::open(session_database);
        if (!store) {
            return std::expected<std::unique_ptr<Controller>, Error>{std::unexpected(Error{
                appellate::app::WorkflowSessionErrorCode::SessionStoreFailure,
                store.error().message,
            })};
        }
        return Controller::reopen(runtime_case.workflow, runtime_case.definition, initial_state,
                                  appellate::storage::AssetStore(session_assets), std::move(*store),
                                  QStringLiteral("engine.resolved-closure.test.1"),
                                  std::move(expected_pins));
    };
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        if (!store) {
            QFAIL(qPrintable(store.error().message));
        }
        const auto reopened = appellate::app::WorkflowSessionController::reopen(
            runtime_case.workflow, runtime_case.definition, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QStringLiteral("engine.resolved-closure.test.1"), *resolved);
        QVERIFY(reopened.has_value());
        QVERIFY((*reopened)->snapshot().pins == pins);
    }
    auto missing_transitive = pins;
    missing_transitive.erase(missing_transitive.begin());
    const auto missing = reopen_with(std::move(missing_transitive));
    QVERIFY(!missing.has_value());
    QCOMPARE(missing.error().code, appellate::app::WorkflowSessionErrorCode::CorruptSession);
    auto changed_transitive = pins;
    changed_transitive.front().digest.front() =
        changed_transitive.front().digest.front() == u'0' ? u'1' : u'0';
    const auto changed = reopen_with(std::move(changed_transitive));
    QVERIFY(!changed.has_value());
    QCOMPARE(changed.error().code, appellate::app::WorkflowSessionErrorCode::CorruptSession);
}

void PackDependencyResolutionTest::resolvesV2CanonicalAuthoritiesAcrossExactDependencies() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto authorities = buildPartitionArchive(
        temporary.path(), QStringLiteral("v2-authorities"), QStringLiteral("test.v2.authorities"),
        {QStringLiteral("resources/authority-set.json")}, {}, false, {}, 2);
    QVERIFY2(authorities.has_value(), authorities ? "" : qPrintable(authorities.error()));
    const auto procedure = buildPartitionArchive(
        temporary.path(), QStringLiteral("v2-procedure"), QStringLiteral("test.v2.procedure"),
        {QStringLiteral("resources/court.json"), QStringLiteral("resources/filing-catalog.json"),
         QStringLiteral("resources/form.json"), QStringLiteral("resources/procedure-profile.json"),
         QStringLiteral("resources/workflow.json")},
        {*authorities}, false, {}, 2);
    QVERIFY2(procedure.has_value(), procedure ? "" : qPrintable(procedure.error()));
    const auto root = buildPartitionArchive(
        temporary.path(), QStringLiteral("v2-root"), QStringLiteral("test.v2.root"),
        {QStringLiteral("resources/argument-config.json"),
         QStringLiteral("resources/bench-configuration.json"),
         QStringLiteral("resources/case.json"), QStringLiteral("resources/judge-profile.json"),
         QStringLiteral("resources/realism-review.json"), QStringLiteral("resources/record.json")},
        {*procedure}, true, {}, 2);
    QVERIFY2(root.has_value(), root ? "" : qPrintable(root.error()));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-authorities"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-procedure"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-root"), 3));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    QCOMPARE(resolved->resourceOwner("example.authorities.fictional"),
             std::optional<PackRevision>(*authorities));

    const auto runtime = appellate::packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QCOMPARE(runtime_case.definition.disposition_targets.size(), std::size_t{2});
    QCOMPARE(runtime_case.definition.disposition_plans.size(), std::size_t{1});
    QCOMPARE(runtime_case.definition.disposition_plans.front().canonical_sha256,
             std::string("eb57e60742d575427f440eb816575a2bc2bd214c2068d79e7a9a7beba2d51a66"));
    QCOMPARE(runtime_case.definition.disposition_plans.front().components.size(), std::size_t{2});
    QVERIFY(std::ranges::any_of(
        runtime_case.definition.disposition_plans.front().components, [](const auto& component) {
            return component.scope == appellate::model::DispositionScope::Part;
        }));
    const auto authored_operation =
        std::ranges::find(runtime_case.workflow.operations,
                          appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
                          &appellate::model::WorkflowOperation::id);
    QVERIFY(authored_operation != runtime_case.workflow.operations.end());
    QCOMPARE(authored_operation->preconditions.size(), std::size_t{1});
    const auto& workflow_authority = runtime_case.workflow.operations.front().authority.primary;
    QVERIFY(workflow_authority.provenance.has_value());
    QCOMPARE(workflow_authority.provenance->source_url,
             std::string("https://example.invalid/rules/2"));
    QCOMPARE(runtime_case.issues.front().authorities.size(), std::size_t{1});
    QVERIFY(runtime_case.issues.front().authorities.front().provenance.has_value());
    QCOMPARE(runtime_case.issues.front().authorities.front().citation,
             std::string("Fictional Rule 1"));
    QCOMPARE(runtime_case.filing_authorities.size(), std::size_t{1});
    QCOMPARE(runtime_case.filing_authorities.front().filing_type_id.value,
             std::string("example.filing.notice"));
    QVERIFY(runtime_case.filing_authorities.front().authority.provenance.has_value());
    QCOMPARE(runtime_case.filing_authorities.front().authority.provenance->locator,
             std::string("Rule 1"));

    const auto session_id = std::string("test.session.v2-authority");
    const auto court_date = appellate::model::LegalDate{std::chrono::year{2026} /
                                                        std::chrono::January / std::chrono::day{2}};
    const auto initial_state = appellate::model::WorkflowState{
        session_id,
        runtime_case.workflow.id,
        runtime_case.workflow.initial_stage_id,
        1,
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
    const auto session_database =
        QDir(temporary.path()).filePath(QStringLiteral("v2-sessions.sqlite"));
    const auto session_assets = QDir(temporary.path()).filePath(QStringLiteral("v2-assets"));
    constexpr auto session_engine = "engine.canonical-authority.test.1";

    auto stripped_workflow_for_create = runtime_case.workflow;
    for (auto& operation : stripped_workflow_for_create.operations) {
        operation.authority.primary.provenance.reset();
        for (auto& supporting : operation.authority.supporting) {
            supporting.provenance.reset();
        }
    }
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto unsafe = appellate::app::WorkflowSessionController::create(
            stripped_workflow_for_create, runtime_case.definition, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QString::fromLatin1(session_engine), QStringLiteral("2026-01-02T11:59:00Z"), *resolved);
        QVERIFY(!unsafe.has_value());
        QCOMPARE(unsafe.error().code,
                 appellate::app::WorkflowSessionErrorCode::InvalidConfiguration);
    }
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto missing = (*store)->loadSession(QString::fromStdString(session_id));
        QVERIFY(!missing.has_value());
        QCOMPARE(missing.error().code, appellate::storage::StoreErrorCode::NotFound);
    }

    const QByteArray document("canonical v2 notice");
    const auto command = appellate::model::SubmitWorkflowFiling{
        appellate::model::WorkflowCommandHeader{
            session_id,
            appellate::model::WorkflowCommandId{"test.command.v2-notice"},
            appellate::model::ActorId{"example.actor.appellant"},
            appellate::model::LegalTime{
                std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}} +
                    std::chrono::hours{12},
                court_date,
            },
        },
        appellate::model::WorkflowFilingId{"test.filing.v2-notice"},
        appellate::model::FilingTypeId{"example.filing.notice"},
        sha256(document).toStdString(),
        {appellate::model::WorkflowFieldValue{
            appellate::model::FilingFieldId{"example.field.caption"}, "caption supplied"}},
        {appellate::model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    std::optional<appellate::model::WorkflowState> persisted_structured_state;
    std::optional<appellate::storage::SessionSnapshot> persisted_structured_snapshot;
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        auto created = appellate::app::WorkflowSessionController::create(
            runtime_case.definition.id, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QString::fromLatin1(session_engine), QStringLiteral("2026-01-02T11:59:00Z"), *resolved);
        QVERIFY2(created.has_value(), created ? "" : qPrintable(created.error().message));
        QCOMPARE((*created)->snapshot().authority_contract,
                 appellate::storage::SessionAuthorityContract::CanonicalV2);
        const auto submitted = (*created)->submit(command, QByteArrayView(document),
                                                  QStringLiteral("2026-01-02T12:00:00Z"));
        QVERIFY2(submitted.has_value(), submitted ? "" : qPrintable(submitted.error().message));
        QVERIFY(!submitted->events.empty());
        QVERIFY(std::ranges::all_of(submitted->events, [](const auto& event) {
            return std::visit(
                [](const auto& concrete) {
                    return concrete.header.authority.primary.provenance.has_value();
                },
                event);
        }));

        QVERIFY(runtime_case.definition.authored_disposition_plan_id.has_value());
        QVERIFY(runtime_case.definition.authored_disposition_operation_id.has_value());
        const auto judgment_date = appellate::model::LegalDate{
            std::chrono::year{2026} / std::chrono::January / std::chrono::day{3}};
        const QByteArray judgment_document("canonical v2 structured judgment");
        const auto judgment = appellate::model::IssueWorkflowJudgment{
            appellate::model::WorkflowCommandHeader{
                session_id,
                appellate::model::WorkflowCommandId{"test.command.v2-judgment"},
                appellate::model::ActorId{"example.actor.court"},
                appellate::model::LegalTime{
                    std::chrono::sys_seconds{std::chrono::sys_days{judgment_date.value}} +
                        std::chrono::hours{14},
                    judgment_date,
                },
            },
            *runtime_case.definition.authored_disposition_operation_id,
            sha256(judgment_document).toStdString(),
            *runtime_case.definition.authored_disposition_plan_id,
        };
        const auto judged = (*created)->submit(appellate::model::WorkflowCommand{judgment},
                                               QByteArrayView(judgment_document),
                                               QStringLiteral("2026-01-03T14:00:00Z"));
        QVERIFY2(judged.has_value(), judged ? "" : qPrintable(judged.error().message));
        QCOMPARE(judged->events.size(), std::size_t{1});
        const auto& judgment_event =
            std::get<appellate::model::WorkflowJudgmentIssued>(judged->events.front());
        QCOMPARE(judgment_event.header.preconditions, authored_operation->preconditions);
        const auto* disposition =
            std::get_if<appellate::model::DispositionPlan>(&judgment_event.disposition);
        QVERIFY(disposition != nullptr);
        QCOMPARE(*disposition, runtime_case.definition.disposition_plans.front());
        QCOMPARE(disposition->components.size(), std::size_t{2});
        QVERIFY(std::ranges::any_of(disposition->components, [](const auto& component) {
            return component.scope == appellate::model::DispositionScope::Part;
        }));

        QVERIFY((*created)->state().judgment_disposition.has_value());
        const auto* persisted_disposition = std::get_if<appellate::model::DispositionPlan>(
            &*(*created)->state().judgment_disposition);
        QVERIFY(persisted_disposition != nullptr);
        QCOMPARE(*persisted_disposition, *disposition);
        QCOMPARE((*created)->snapshot().commands.size(), std::size_t{2});
        QCOMPARE((*created)->snapshot().events.size(), std::size_t{3});
        QCOMPARE((*created)->snapshot().sequence, qint64{3});
        QCOMPARE((*created)->snapshot().asset_references.size(), std::size_t{2});
        QCOMPARE(QJsonDocument::fromJson((*created)->snapshot().commands.back().payload_json)
                     .object()
                     .value(QStringLiteral("schema_version"))
                     .toInt(),
                 3);
        const auto persisted_event =
            QJsonDocument::fromJson((*created)->snapshot().events.back().payload_json).object();
        QCOMPARE(persisted_event.value(QStringLiteral("schema_version")).toInt(), 3);
        const auto persisted_payload = persisted_event.value(QStringLiteral("payload")).toObject();
        QCOMPARE(persisted_payload.value(QStringLiteral("preconditions")).toArray().size(), 1);
        QCOMPARE(
            persisted_payload.value(QStringLiteral("disposition"))
                .toObject()
                .value(QStringLiteral("digest"))
                .toString(),
            QStringLiteral("eb57e60742d575427f440eb816575a2bc2bd214c2068d79e7a9a7beba2d51a66"));
        persisted_structured_state = (*created)->state();
        persisted_structured_snapshot = (*created)->snapshot();
    }

    const auto pins = appellate::app::revisionPinsForSession(*resolved);
    auto stripped_workflow = runtime_case.workflow;
    for (auto& operation : stripped_workflow.operations) {
        operation.authority.primary.provenance.reset();
        for (auto& supporting : operation.authority.supporting) {
            supporting.provenance.reset();
        }
    }
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto downgraded = appellate::app::WorkflowSessionController::reopen(
            stripped_workflow, runtime_case.definition, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QString::fromLatin1(session_engine), pins);
        QVERIFY(!downgraded.has_value());
        QCOMPARE(downgraded.error().code,
                 appellate::app::WorkflowSessionErrorCode::InvalidConfiguration);
    }
    auto legacy_workflow = stripped_workflow;
    for (auto& operation : legacy_workflow.operations) {
        operation.preconditions.clear();
    }
    auto legacy_case = runtime_case.definition;
    legacy_case.disposition_targets.clear();
    legacy_case.disposition_plans.clear();
    legacy_case.authored_disposition_plan_id.reset();
    legacy_case.authored_disposition_operation_id.reset();
    {
        auto store = appellate::storage::SessionStore::open(session_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto downgraded = appellate::app::WorkflowSessionController::reopen(
            legacy_workflow, legacy_case, initial_state,
            appellate::storage::AssetStore(session_assets), std::move(*store),
            QString::fromLatin1(session_engine), pins);
        QVERIFY(!downgraded.has_value());
        QCOMPARE(downgraded.error().code, appellate::app::WorkflowSessionErrorCode::CorruptSession);
    }

    const auto reopen_canonical = [&]() {
        using Controller = appellate::app::WorkflowSessionController;
        using Error = appellate::app::WorkflowSessionError;
        auto store = appellate::storage::SessionStore::open(session_database);
        if (!store) {
            return std::expected<std::unique_ptr<Controller>, Error>{std::unexpected(Error{
                appellate::app::WorkflowSessionErrorCode::SessionStoreFailure,
                store.error().message,
            })};
        }
        return Controller::reopen(runtime_case.definition.id, initial_state,
                                  appellate::storage::AssetStore(session_assets), std::move(*store),
                                  QString::fromLatin1(session_engine), *resolved);
    };
    auto reopened = reopen_canonical();
    QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error().message));
    QCOMPARE((*reopened)->snapshot().authority_contract,
             appellate::storage::SessionAuthorityContract::CanonicalV2);
    QVERIFY(persisted_structured_state.has_value());
    QVERIFY(persisted_structured_snapshot.has_value());
    QCOMPARE((*reopened)->state(), *persisted_structured_state);
    const auto& reopened_snapshot = (*reopened)->snapshot();
    QCOMPARE(reopened_snapshot.session_id, persisted_structured_snapshot->session_id);
    QCOMPARE(reopened_snapshot.engine_revision, persisted_structured_snapshot->engine_revision);
    QCOMPARE(reopened_snapshot.authority_contract,
             persisted_structured_snapshot->authority_contract);
    QCOMPARE(reopened_snapshot.sequence, persisted_structured_snapshot->sequence);
    QCOMPARE(reopened_snapshot.pins, persisted_structured_snapshot->pins);
    QCOMPARE(reopened_snapshot.commands, persisted_structured_snapshot->commands);
    QCOMPARE(reopened_snapshot.events, persisted_structured_snapshot->events);
    QCOMPARE(reopened_snapshot.docket, persisted_structured_snapshot->docket);
    QCOMPARE(reopened_snapshot.asset_references, persisted_structured_snapshot->asset_references);
    (*reopened).reset();

    const auto original_event =
        sessionEventPayload(session_database, QString::fromStdString(session_id), 1);
    QVERIFY(original_event.has_value());
    const auto bad_url = mutateAuthorityProvenance(*original_event, [](QJsonObject& provenance) {
        provenance.insert(QStringLiteral("source_url"),
                          QStringLiteral("https://Example.invalid/rules/1"));
    });
    QVERIFY(
        writeSessionEventPayload(session_database, QString::fromStdString(session_id), 1, bad_url));
    QVERIFY(!reopen_canonical().has_value());
    QVERIFY(writeSessionEventPayload(session_database, QString::fromStdString(session_id), 1,
                                     *original_event));

    const auto changed_status =
        mutateAuthorityProvenance(*original_event, [](QJsonObject& provenance) {
            provenance.insert(QStringLiteral("precedential_status"),
                              QStringLiteral("precedential"));
        });
    QVERIFY(writeSessionEventPayload(session_database, QString::fromStdString(session_id), 1,
                                     changed_status));
    QVERIFY(!reopen_canonical().has_value());
    QVERIFY(writeSessionEventPayload(session_database, QString::fromStdString(session_id), 1,
                                     *original_event));

    const auto original_judgment_event =
        sessionEventPayload(session_database, QString::fromStdString(session_id), 3);
    QVERIFY(original_judgment_event.has_value());
    auto judgment_envelope = QJsonDocument::fromJson(*original_judgment_event).object();
    auto judgment_payload = judgment_envelope.value(QStringLiteral("payload")).toObject();
    auto stored_plan = judgment_payload.value(QStringLiteral("disposition")).toObject();
    auto changed_digest = stored_plan.value(QStringLiteral("digest")).toString();
    changed_digest[0] = changed_digest.at(0) == u'0' ? u'1' : u'0';
    stored_plan.insert(QStringLiteral("digest"), changed_digest);
    judgment_payload.insert(QStringLiteral("disposition"), stored_plan);
    judgment_envelope.insert(QStringLiteral("payload"), judgment_payload);
    QVERIFY(
        writeSessionEventPayload(session_database, QString::fromStdString(session_id), 3,
                                 QJsonDocument(judgment_envelope).toJson(QJsonDocument::Compact)));
    QVERIFY(!reopen_canonical().has_value());
    QVERIFY(writeSessionEventPayload(session_database, QString::fromStdString(session_id), 3,
                                     *original_judgment_event));

    judgment_envelope = QJsonDocument::fromJson(*original_judgment_event).object();
    judgment_payload = judgment_envelope.value(QStringLiteral("payload")).toObject();
    auto stored_preconditions = judgment_payload.value(QStringLiteral("preconditions")).toArray();
    QCOMPARE(stored_preconditions.size(), 1);
    auto stored_precondition = stored_preconditions.first().toObject();
    stored_precondition.insert(QStringLiteral("present"), false);
    stored_preconditions.replace(0, stored_precondition);
    judgment_payload.insert(QStringLiteral("preconditions"), stored_preconditions);
    judgment_envelope.insert(QStringLiteral("payload"), judgment_payload);
    QVERIFY(
        writeSessionEventPayload(session_database, QString::fromStdString(session_id), 3,
                                 QJsonDocument(judgment_envelope).toJson(QJsonDocument::Compact)));
    QVERIFY(!reopen_canonical().has_value());
    QVERIFY(writeSessionEventPayload(session_database, QString::fromStdString(session_id), 3,
                                     *original_judgment_event));

    reopened = reopen_canonical();
    QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error().message));
}

void PackDependencyResolutionTest::rejectsSiblingAssistedDependencyReference() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto leaf = buildPartitionArchive(
        temporary.path(), QStringLiteral("leaf"), QStringLiteral("test.visibility.leaf"),
        {QStringLiteral("resources/authority-set.json"), QStringLiteral("resources/court.json")});
    QVERIFY(leaf.has_value());
    const auto sibling = buildPartitionArchive(
        temporary.path(), QStringLiteral("sibling"), QStringLiteral("test.visibility.sibling"),
        {QStringLiteral("resources/filing-catalog.json"), QStringLiteral("resources/form.json"),
         QStringLiteral("resources/workflow.json")},
        {*leaf});
    QVERIFY(sibling.has_value());
    const auto dependent = buildPartitionArchive(
        temporary.path(), QStringLiteral("dependent"), QStringLiteral("test.visibility.dependent"),
        {QStringLiteral("resources/procedure-profile.json")}, {*leaf});
    QVERIFY(dependent.has_value());

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("leaf"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("sibling"), 2));
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("dependent")),
                                   QStringLiteral("2026-08-11T00:00:03Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::InvalidResolvedGraph);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{2});
    QCOMPARE(QDir((*catalog)->archivesDirectory())
                 .entryList(QStringList{QStringLiteral("*.awpack")}, QDir::Files)
                 .size(),
             2);
}

void PackDependencyResolutionTest::rejectsWrongExactDigestForBlobStreaming() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildBlobArchive(temporary.path(), QStringLiteral("blob"),
                         QStringLiteral("test.exact.blob"), QByteArray("exact revision"));
    QVERIFY(revision.has_value());
    const auto archive = archivePath(temporary.path(), QStringLiteral("blob"));
    const auto loaded = PackArchive::importArchive(archive);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->blobs.size(), std::size_t{1});
    auto wrong_revision = *revision;
    wrong_revision.digest.front() = wrong_revision.digest.front() == '0' ? '1' : '0';
    QBuffer destination;
    QVERIFY(destination.open(QIODevice::ReadWrite));
    const auto streamed = PackArchive::streamValidatedBlob(archive, wrong_revision,
                                                           loaded->blobs.front(), destination);
    QVERIFY(!streamed.has_value());
    QCOMPARE(streamed.error().code, appellate::packs::ErrorCode::InvalidManifest);
    QCOMPARE(destination.size(), qint64{0});
}

void PackDependencyResolutionTest::serializesPublicationAcrossCatalogInstances() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildBlobArchive(temporary.path(), QStringLiteral("locked"),
                         QStringLiteral("test.locked.root"), QByteArray("serialized publication"));
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());

    QLockFile competing_install(QDir(catalog_root).filePath(QStringLiteral(".install.lock")));
    QVERIFY(competing_install.tryLock());
    const auto blocked =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("locked")),
                                   QStringLiteral("2026-08-11T00:00:01Z"));
    QVERIFY(!blocked.has_value());
    QCOMPARE(blocked.error().code, CatalogErrorCode::CannotStoreArchive);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());
    const auto every_file = QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot;
    QVERIFY(QDir((*catalog)->archivesDirectory()).entryList({}, every_file).empty());
    QVERIFY(QDir((*catalog)->blobObjectsDirectory()).entryList({}, every_file).empty());

    competing_install.unlock();
    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("locked")),
                                   QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY(installed.has_value());
    QCOMPARE(installed->revision, *revision);
}

void PackDependencyResolutionTest::rollsBackNewArchiveAndBlobAfterFinalizationFailure() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision =
        buildBlobArchive(temporary.path(), QStringLiteral("rollback"),
                         QStringLiteral("test.rollback.root"), QByteArray("new durable objects"));
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    auto catalog = PackCatalog::open(catalog_root);
    QVERIFY(catalog.has_value());
    QVERIFY(executeCatalogSql(
        catalog_root,
        QStringLiteral("CREATE TRIGGER reject_test_root BEFORE INSERT ON pack_revisions "
                       "WHEN NEW.pack_id = 'test.rollback.root' BEGIN "
                       "SELECT RAISE(FAIL, 'injected post-finalization failure'); END")));

    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("rollback")),
                                   QStringLiteral("2026-08-11T00:00:01Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::QueryFailed);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());

    const auto every_file = QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot;
    QVERIFY(QDir((*catalog)->archivesDirectory()).entryList({}, every_file).empty());
    QVERIFY(QDir((*catalog)->blobObjectsDirectory()).entryList({}, every_file).empty());

    QVERIFY(executeCatalogSql(catalog_root, QStringLiteral("DROP TRIGGER reject_test_root")));
    const auto retried =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("rollback")),
                                   QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY(retried.has_value());
    QCOMPARE(retried->revision, *revision);
}

} // namespace

QTEST_MAIN(PackDependencyResolutionTest)

#include "tst_pack_dependency_resolution.moc"
