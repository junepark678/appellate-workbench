#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/pack_version.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cstdint>
#include <optional>

namespace {

class PackReaderTest final : public QObject {
    Q_OBJECT

  private slots:
    void validatesSchemaSpecificPackVersions_data();
    void validatesSchemaSpecificPackVersions();
    void rejectsCalendarVersionInV1Manifest();
    void rejectsInvalidCalendarVersionInV2Manifest();
    void runtimeEnforcesSchemaSpecificPackVersions();
    void loadsValidPack();
    void loadsFullDeclarativeResourceGraph();
    void rejectsMissingBlobArray();
    void rejectsDuplicateAndOverlappingBlobPaths();
    void rejectsInvalidBlob_data();
    void rejectsInvalidBlob();
    void rejectsBlobSizeBudgets();
    void rejectsUnlistedAndOrphanBlobs();
    void rejectsMissingAndMismatchedRecordBlobs();
    void acceptsLegacyRecordWithoutOptionalMetadata();
    void rejectsInvalidRecordMetadataGraph();
    void validatesUnicodeScalarLength();
    void digestIncludesBlobDescriptor();
    void rejectsMalformedJson();
    void rejectsUnsupportedSchema();
    void rejectsPathTraversal();
    void rejectsDuplicateContentId();
    void rejectsUnknownManifestField();
    void rejectsProhibitedJudgeField();
    void rejectsInvalidStructuredVoice();
    void rejectsInvalidIdentifiersVersionsAndHashes();
    void rejectsMalformedCapabilityAndDependency();
    void rejectsDuplicateContentPath();
    void rejectsDuplicatePayloadId();
    void rejectsUndeclaredFile();
    void rejectsIntermediateSymlink();
    void rejectsOversizedJson();
    void producesCanonicalOrderIndependentDigest();
    void digestIncludesPathAndDependencies();
    void rejectsDuplicateJsonKeys();
    void rejectsUnknownResourceKind();
    void rejectsUnsupportedResourceSchema();
    void rejectsGenericSchemaViolation();
    void rejectsDescriptorPayloadDisagreement();
    void rejectsBrokenCrossReference();
    void rejectsIncompleteWorkflowAuthority();
    void rejectsConflictingWorkflowAuthority();
    void acceptsCatalogSupersetAndStagesWithoutFallbackRejections();
    void rejectsWorkflowInvariantViolation();
    void loadsWorkflowCapabilitySliceAndFencesCoverage();
    void rejectsForgedWorkflowCapabilityGraphs();
};

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

[[nodiscard]] QByteArray jsonBytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool writeBytes(const QString& root, const QString& relative_path,
                              const QByteArray& bytes) {
    const QFileInfo info(QDir(root).filePath(relative_path));
    if (!QDir{}.mkpath(info.path())) {
        return false;
    }
    QFile file(info.filePath());
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == static_cast<qint64>(bytes.size());
}

[[nodiscard]] bool writeJson(const QString& root, const QString& relative_path,
                             const QJsonObject& object) {
    return writeBytes(root, relative_path, jsonBytes(object));
}

[[nodiscard]] bool copyTree(const QString& source, const QString& destination) {
    QDirIterator iterator(source, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir source_root(source);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        QFile input(source_path);
        if (!input.open(QIODevice::ReadOnly) ||
            !writeBytes(destination, source_root.relativeFilePath(source_path), input.readAll())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool replaceResourceField(const QString& root, const QString& relative_path,
                                        const QString& field, const QJsonValue& value) {
    QFile resource_file(QDir(root).filePath(relative_path));
    if (!resource_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto resource = QJsonDocument::fromJson(resource_file.readAll()).object();
    resource_file.close();
    resource.insert(field, value);
    const auto resource_bytes = jsonBytes(resource);
    if (!writeBytes(root, relative_path, resource_bytes)) {
        return false;
    }

    QFile manifest_file(QDir(root).filePath(QStringLiteral("manifest.json")));
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    bool found = false;
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto entry = contents.at(index).toObject();
        if (entry.value(QStringLiteral("path")).toString() != relative_path) {
            continue;
        }
        entry.insert(QStringLiteral("sha256"), sha256(resource_bytes));
        contents.replace(index, entry);
        found = true;
        break;
    }
    manifest.insert(QStringLiteral("contents"), contents);
    return found && writeJson(root, QStringLiteral("manifest.json"), manifest);
}

[[nodiscard]] bool replaceResourceDocument(const QString& root, const QString& relative_path,
                                           const QJsonObject& resource) {
    const auto resource_bytes = jsonBytes(resource);
    if (!writeBytes(root, relative_path, resource_bytes)) {
        return false;
    }
    QFile manifest_file(QDir(root).filePath(QStringLiteral("manifest.json")));
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto entry = contents.at(index).toObject();
        if (entry.value(QStringLiteral("path")).toString() != relative_path) {
            continue;
        }
        entry.insert(QStringLiteral("sha256"), sha256(resource_bytes));
        contents.replace(index, entry);
        manifest.insert(QStringLiteral("contents"), contents);
        return writeJson(root, QStringLiteral("manifest.json"), manifest);
    }
    return false;
}

[[nodiscard]] bool setCapability(const QString& root, const QString& capability_id,
                                 std::optional<int> version) {
    QFile manifest_file(QDir(root).filePath(QStringLiteral("manifest.json")));
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    QJsonArray changed;
    bool found = false;
    for (const auto& value : capabilities) {
        auto capability = value.toObject();
        if (capability.value(QStringLiteral("id")).toString() == capability_id) {
            found = true;
            if (version.has_value()) {
                capability.insert(QStringLiteral("version"), *version);
                changed.push_back(capability);
            }
        } else {
            changed.push_back(capability);
        }
    }
    if (!found && version.has_value()) {
        changed.push_back(QJsonObject{{QStringLiteral("id"), capability_id},
                                      {QStringLiteral("version"), *version}});
    }
    manifest.insert(QStringLiteral("required_capabilities"), changed);
    return writeJson(root, QStringLiteral("manifest.json"), manifest);
}

[[nodiscard]] bool configureWorkflowCapabilities(const QString& root) {
    const auto relative_path = QStringLiteral("resources/workflow.json");
    QFile workflow_file(QDir(root).filePath(relative_path));
    if (!workflow_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto workflow = QJsonDocument::fromJson(workflow_file.readAll()).object();
    workflow_file.close();
    auto operations = workflow.value(QStringLiteral("operations")).toArray();
    qsizetype judgment_index = -1;
    for (qsizetype index = 0; index < operations.size(); ++index) {
        if (operations.at(index).toObject().value(QStringLiteral("operation_id")).toString() ==
            QStringLiteral("example.operation.issue-judgment")) {
            judgment_index = index;
            break;
        }
    }
    if (judgment_index < 0) {
        return false;
    }
    constexpr auto record_sha = "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366";
    auto judgment = operations.at(judgment_index).toObject();
    auto order = judgment;
    order.insert(QStringLiteral("operation_id"),
                 QStringLiteral("example.operation.enter-bound-order"));
    order.insert(QStringLiteral("opcode"), QStringLiteral("enter_order"));
    order.remove(QStringLiteral("preconditions"));
    order.insert(
        QStringLiteral("document_binding"),
        QJsonObject{{QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                    {QStringLiteral("document_sha256"), QString::fromLatin1(record_sha)},
                    {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-02")},
                    {QStringLiteral("order_id"), QStringLiteral("example.order.bound")},
                    {QStringLiteral("disposition"), QStringLiteral("granted")}});
    operations.push_back(order);

    auto preconditions = judgment.value(QStringLiteral("preconditions")).toArray();
    preconditions.push_back(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("filing_instance")},
        {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
        {QStringLiteral("present"), true},
        {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
        {QStringLiteral("filing_id"), QStringLiteral("example.filing.notice-one")},
        {QStringLiteral("accept_operation_id"), QStringLiteral("example.operation.accept-notice")},
        {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
        {QStringLiteral("document_sha256"), QString::fromLatin1(record_sha)}});
    preconditions.push_back(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("order_instance")},
        {QStringLiteral("order_id"), QStringLiteral("example.order.bound")},
        {QStringLiteral("disposition"), QStringLiteral("granted")},
        {QStringLiteral("operation_id"), QStringLiteral("example.operation.enter-bound-order")},
        {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
        {QStringLiteral("document_sha256"), QString::fromLatin1(record_sha)}});
    judgment.insert(QStringLiteral("preconditions"), preconditions);
    judgment.insert(
        QStringLiteral("document_binding"),
        QJsonObject{
            {QStringLiteral("record_entry_id"), QStringLiteral("example.record.brief-opening")},
            {QStringLiteral("document_sha256"), QString::fromLatin1(record_sha)},
            {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-03")}});
    judgment.insert(QStringLiteral("disposition_plan_id"),
                    QStringLiteral("example.disposition.fictional"));
    operations.replace(judgment_index, judgment);
    workflow.insert(QStringLiteral("operations"), operations);

    auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
    if (routes.isEmpty()) {
        return false;
    }
    auto route = routes.at(0).toObject();
    route.insert(QStringLiteral("authorized_role_scope"), QStringLiteral("catalog_subset"));
    route.insert(
        QStringLiteral("deficiency_deadline"),
        QJsonObject{
            {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.notice-cure-exact")},
            {QStringLiteral("operation_id"), QStringLiteral("example.operation.calculate-cure")},
            {QStringLiteral("id_mode"), QStringLiteral("exact")},
            {QStringLiteral("trigger_filing"),
             QJsonObject{
                 {QStringLiteral("filing_id"), QStringLiteral("example.filing.notice-deficient")},
                 {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
                 {QStringLiteral("record_entry_id"),
                  QStringLiteral("example.record.brief-opening")},
                 {QStringLiteral("document_sha256"), QString::fromLatin1(record_sha)},
                 {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-03")}}}});
    route.insert(QStringLiteral("satisfies_deadline_id"),
                 QStringLiteral("example.deadline.notice-cure-exact"));
    routes.replace(0, route);
    workflow.insert(QStringLiteral("filing_routes"), routes);
    if (!replaceResourceDocument(root, relative_path, workflow)) {
        return false;
    }
    for (const auto* capability_id :
         {"workbench.pack.route-role-subsets", "workbench.pack.workflow-instance-preconditions",
          "workbench.pack.static-deficiency-deadlines",
          "workbench.pack.operation-document-bindings",
          "workbench.pack.operation-disposition-bindings"}) {
        if (!setCapability(root, QString::fromLatin1(capability_id), 1)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] QJsonObject validJudge(const QString& id = QStringLiteral("example.judge.measured"),
                                     const QString& name = QStringLiteral("Measured Panelist")) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("resource_kind"), QStringLiteral("judge_profile")},
        {QStringLiteral("resource_id"), id},
        {QStringLiteral("display_name"), name},
        {QStringLiteral("profile_class"), QStringLiteral("fictional_composite")},
        {QStringLiteral("compatibility"),
         QJsonObject{
             {QStringLiteral("court_roles"), QJsonArray{QStringLiteral("appellate")}},
             {QStringLiteral("jurisdiction_ids"), QJsonArray{QStringLiteral("us.ca4")}},
         }},
        {QStringLiteral("interaction"),
         QJsonObject{
             {QStringLiteral("directness"), 0.62},
             {QStringLiteral("formality"), 0.78},
             {QStringLiteral("question_length"), 0.52},
             {QStringLiteral("interruption_frequency"), 0.28},
             {QStringLiteral("follow_up_depth"), 0.71},
             {QStringLiteral("hypothetical_frequency"), 0.44},
             {QStringLiteral("concession_recall"), 0.73},
             {QStringLiteral("record_pin_demand"), 0.81},
             {QStringLiteral("time_strictness"), 0.66},
             {QStringLiteral("issue_focus"),
              QJsonArray{QJsonObject{
                  {QStringLiteral("topic_id"), QStringLiteral("appellate.issue.preservation")},
                  {QStringLiteral("weight"), 0.82},
              }}},
         }},
        {QStringLiteral("voice"),
         QJsonObject{
             {QStringLiteral("register"), QStringLiteral("formal")},
             {QStringLiteral("cadence"), QStringLiteral("measured")},
             {QStringLiteral("question_framing"), QStringLiteral("socratic")},
             {QStringLiteral("address_convention"), QStringLiteral("counsel")},
             {QStringLiteral("verbosity"), 0.46},
             {QStringLiteral("sentence_complexity"), 0.58},
             {QStringLiteral("question_phrases"),
              QJsonArray{QStringLiteral("help the court with this point"),
                         QStringLiteral("state the limiting principle")}},
             {QStringLiteral("interruption_phrases"),
              QJsonArray{QStringLiteral("pause there"), QStringLiteral("before you continue")}},
             {QStringLiteral("clarification_phrases"),
              QJsonArray{QStringLiteral("clarify that answer"),
                         QStringLiteral("be precise about the record")}},
         }},
    };
}

[[nodiscard]] QJsonObject contentEntry(const QString& id, const QString& path,
                                       const QByteArray& payload) {
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), QStringLiteral("judge_profile")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("path"), path},
        {QStringLiteral("sha256"), sha256(payload)},
    };
}

[[nodiscard]] QByteArray validPdf() {
    return QByteArray("%PDF-1.7\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n");
}

[[nodiscard]] QJsonObject blobEntry(const QString& path, const QByteArray& payload) {
    return QJsonObject{
        {QStringLiteral("path"), path},
        {QStringLiteral("media_type"), QStringLiteral("application/pdf")},
        {QStringLiteral("byte_size"), static_cast<qint64>(payload.size())},
        {QStringLiteral("sha256"), sha256(payload)},
    };
}

[[nodiscard]] QJsonObject capability(const QString& id, int version = 1) {
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("version"), version},
    };
}

[[nodiscard]] QJsonObject dependency(const QString& id, const QString& version,
                                     QChar digest_character) {
    return QJsonObject{
        {QStringLiteral("pack_id"), id},
        {QStringLiteral("version"), version},
        {QStringLiteral("sha256"), QString(64, digest_character)},
    };
}

[[nodiscard]] QJsonObject validManifest(const QJsonArray& contents,
                                        const QJsonArray& capabilities = {},
                                        const QJsonArray& dependencies = {},
                                        const QString& version = QStringLiteral("1.0.0"),
                                        const QJsonArray& blobs = {}) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("pack_id"), QStringLiteral("example.test.pack")},
        {QStringLiteral("version"), version},
        {QStringLiteral("required_capabilities"), capabilities},
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("contents"), contents},
        {QStringLiteral("blobs"), blobs},
    };
}

[[nodiscard]] bool updateFixtureBlob(const QString& root, const QByteArray& payload) {
    const auto blob_path = QStringLiteral("objects/final-order.pdf");
    if (!writeBytes(root, blob_path, payload)) {
        return false;
    }

    const auto record_path = QStringLiteral("resources/record.json");
    QFile record_file(QDir(root).filePath(record_path));
    if (!record_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    record_file.close();
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    auto entry = entries.at(0).toObject();
    entry.insert(QStringLiteral("asset_sha256"), sha256(payload));
    entries.replace(0, entry);
    record.insert(QStringLiteral("docket_entries"), entries);
    const auto record_bytes = jsonBytes(record);
    if (!writeBytes(root, record_path, record_bytes)) {
        return false;
    }

    QFile manifest_file(QDir(root).filePath(QStringLiteral("manifest.json")));
    if (!manifest_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    auto blobs = manifest.value(QStringLiteral("blobs")).toArray();
    auto blob = blobs.at(0).toObject();
    blob.insert(QStringLiteral("byte_size"), static_cast<qint64>(payload.size()));
    blob.insert(QStringLiteral("sha256"), sha256(payload));
    blobs.replace(0, blob);
    manifest.insert(QStringLiteral("blobs"), blobs);
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto content = contents.at(index).toObject();
        if (content.value(QStringLiteral("path")).toString() == record_path) {
            content.insert(QStringLiteral("sha256"), sha256(record_bytes));
            contents.replace(index, content);
            manifest.insert(QStringLiteral("contents"), contents);
            return writeJson(root, QStringLiteral("manifest.json"), manifest);
        }
    }
    return false;
}

void PackReaderTest::validatesSchemaSpecificPackVersions_data() {
    QTest::addColumn<QString>("version");
    QTest::addColumn<int>("manifest_schema_version");
    QTest::addColumn<bool>("valid");

    QTest::newRow("v1-semver") << QStringLiteral("1.2.3") << 1 << true;
    QTest::newRow("v1-semver-prerelease-build")
        << QStringLiteral("1.2.3-rc.1+build.5") << 1 << true;
    QTest::newRow("v1-calendar-leap-rejected") << QStringLiteral("2000.02.29") << 1 << false;
    QTest::newRow("v1-lexical-overlap-is-semver") << QStringLiteral("2026.12.11") << 1 << true;
    QTest::newRow("v1-unpadded-invalid-date-is-semver") << QStringLiteral("2026.2.31") << 1 << true;

    QTest::newRow("v2-semver") << QStringLiteral("1.2.3") << 2 << true;
    QTest::newRow("v2-calendar-first-year-leap") << QStringLiteral("2000.02.29") << 2 << true;
    QTest::newRow("v2-calendar-zero-padded") << QStringLiteral("2026.03.23") << 2 << true;
    QTest::newRow("v2-lexical-overlap-is-semver") << QStringLiteral("2026.12.11") << 2 << true;
    QTest::newRow("v2-pre-range-overlap-is-semver") << QStringLiteral("1999.12.31") << 2 << true;
    QTest::newRow("v2-unpadded-invalid-date-is-semver") << QStringLiteral("2026.2.31") << 2 << true;
    QTest::newRow("v2-non-leap-century") << QStringLiteral("2100.02.29") << 2 << false;
    QTest::newRow("v2-impossible-february-day") << QStringLiteral("2026.02.31") << 2 << false;
    QTest::newRow("v2-impossible-april-day") << QStringLiteral("2026.04.31") << 2 << false;
    QTest::newRow("v2-unambiguous-pre-range") << QStringLiteral("1999.02.03") << 2 << false;
    QTest::newRow("v2-five-digit-year") << QStringLiteral("10000.01.01") << 2 << false;
    QTest::newRow("v2-calendar-suffix") << QStringLiteral("2026.02.03-alpha") << 2 << false;
    QTest::newRow("v2-calendar-width") << QStringLiteral("2026.02.3") << 2 << false;
    QTest::newRow("unsupported-schema") << QStringLiteral("1.2.3") << 3 << false;
}

void PackReaderTest::validatesSchemaSpecificPackVersions() {
    QFETCH(QString, version);
    QFETCH(int, manifest_schema_version);
    QFETCH(bool, valid);

    QCOMPARE(appellate::packs::isValidPackVersion(
                 version, static_cast<std::uint32_t>(manifest_schema_version)),
             valid);
}

void PackReaderTest::rejectsCalendarVersionInV1Manifest() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("minimal-pack")), pack.path()));

    const auto manifest_path = QDir(pack.path()).filePath(QStringLiteral("manifest.json"));
    QFile manifest_file(manifest_path);
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    manifest.insert(QStringLiteral("version"), QStringLiteral("2000.02.29"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));

    const auto loaded = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!loaded.has_value());
    QCOMPARE(loaded.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsInvalidCalendarVersionInV2Manifest() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));

    const auto manifest_path = QDir(pack.path()).filePath(QStringLiteral("manifest.json"));
    QFile manifest_file(manifest_path);
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    manifest.insert(QStringLiteral("version"), QStringLiteral("2100.02.29"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));

    const auto loaded = appellate::packs::PackReader::readDirectory(
        pack.path(), appellate::packs::PackValidationScope::ResolvedClosure);
    QVERIFY(!loaded.has_value());
    QCOMPARE(loaded.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::runtimeEnforcesSchemaSpecificPackVersions() {
    const auto v1 =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    QVERIFY2(v1.has_value(), v1 ? "" : qPrintable(v1.error().message));
    auto forged_v1 = *v1;
    forged_v1.revision.version = "2000.02.29";
    const auto rejected_v1 = appellate::packs::loadRuntimePack(forged_v1);
    QVERIFY(!rejected_v1.has_value());
    QCOMPARE(rejected_v1.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);

    const auto v2 = appellate::packs::PackReader::readDirectory(
        fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(v2.has_value(), v2 ? "" : qPrintable(v2.error().message));
    auto forged_v2 = *v2;
    forged_v2.revision.version = "2100.02.29";
    const auto rejected_v2 = appellate::packs::loadRuntimePack(forged_v2);
    QVERIFY(!rejected_v2.has_value());
    QCOMPARE(rejected_v2.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);

    auto valid_calendar_v2 = *v2;
    valid_calendar_v2.revision.version = "2000.02.29";
    const auto accepted_v2 = appellate::packs::loadRuntimePack(valid_calendar_v2);
    QVERIFY2(accepted_v2.has_value(), accepted_v2 ? "" : accepted_v2.error().message.c_str());
    QCOMPARE(accepted_v2->revision.version, std::string("2000.02.29"));
}

void PackReaderTest::loadsValidPack() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("minimal-pack")));

    if (!result.has_value()) {
        QFAIL(qPrintable(result.error().message));
    }
    QCOMPARE(result->revision.id.value, std::string("example.appellate.ca4"));
    QCOMPARE(result->revision.version, std::string("0.1.0"));
    QCOMPARE(result->required_capabilities.size(), std::size_t{2});
    QCOMPARE(result->dependencies.size(), std::size_t{1});
    QCOMPARE(result->dependencies.front().revision.id.value,
             std::string("example.foundation.common"));
    QCOMPARE(result->resources.size(), std::size_t{1});
    QCOMPARE(result->judge_profiles.size(), std::size_t{1});
    const auto& profile = result->judge_profiles.front();
    QCOMPARE(profile.display_name, std::string("Measured Panelist"));
    QCOMPARE(profile.profile_class, appellate::model::ProfileClass::FictionalComposite);
    QCOMPARE(profile.compatibility.court_roles.size(), std::size_t{1});
    QCOMPARE(profile.compatibility.jurisdiction_ids.front(), std::string("us.ca4"));
    QCOMPARE(profile.interaction.issue_focus.size(), std::size_t{2});
    QCOMPARE(profile.interaction.record_pin_demand, 0.81);
    QCOMPARE(profile.voice.cadence, appellate::model::VoiceCadence::Measured);
    QCOMPARE(profile.voice.question_framing, appellate::model::QuestionFraming::Socratic);
    QCOMPARE(profile.voice.address_convention, appellate::model::CounselAddress::Counsel);
    QCOMPARE(profile.voice.question_phrases.size(), std::size_t{2});
    QCOMPARE(profile.voice.interruption_phrases.size(), std::size_t{2});
    QCOMPARE(profile.voice.clarification_phrases.size(), std::size_t{2});
}

void PackReaderTest::loadsFullDeclarativeResourceGraph() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));

    if (!result.has_value()) {
        QFAIL(qPrintable(result.error().message));
    }
    QCOMPARE(result->revision.id.value, std::string("example.full.fictional"));
    QCOMPARE(result->resources.size(), std::size_t{12});
    QCOMPARE(result->blobs.size(), std::size_t{1});
    QCOMPARE(result->blobs.front().path, std::string("objects/final-order.pdf"));
    QCOMPARE(result->blobs.front().media_type, std::string("application/pdf"));
    QCOMPARE(result->blobs.front().byte_size, std::uint64_t{1163});
    QCOMPARE(result->judge_profiles.size(), std::size_t{1});
    QSet<int> kinds;
    for (const auto& resource : result->resources) {
        kinds.insert(static_cast<int>(resource.descriptor.kind));
        QCOMPARE(resource.document.value(QStringLiteral("resource_id")).toString().toStdString(),
                 resource.descriptor.id);
    }
    QCOMPARE(kinds.size(), 12);
    QCOMPARE(result->resources.front().descriptor.id, std::string("example.argument.fictional"));
}

void PackReaderTest::rejectsMissingBlobArray() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), judge));
    auto manifest = validManifest(QJsonArray{contentEntry(
        QStringLiteral("example.judge.measured"), QStringLiteral("judges/measured.json"), judge)});
    manifest.remove(QStringLiteral("blobs"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsDuplicateAndOverlappingBlobPaths() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto content_path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), content_path, judge));
    const QJsonArray contents{
        contentEntry(QStringLiteral("example.judge.measured"), content_path, judge)};
    const auto pdf = validPdf();
    const auto blob = blobEntry(QStringLiteral("objects/order.pdf"), pdf);

    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(contents, {}, {}, QStringLiteral("1.0.0"), QJsonArray{blob, blob})));
    auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentPath);

    auto overlapping = blob;
    overlapping.insert(QStringLiteral("path"), content_path + QStringLiteral("/order.pdf"));
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(contents, {}, {}, QStringLiteral("1.0.0"), QJsonArray{overlapping})));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentPath);

    overlapping.insert(QStringLiteral("path"), content_path);
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(contents, {}, {}, QStringLiteral("1.0.0"), QJsonArray{overlapping})));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentPath);
}

void PackReaderTest::rejectsInvalidBlob_data() {
    QTest::addColumn<QString>("failure");
    QTest::addColumn<int>("expected_code");
    QTest::newRow("wrong hash") << QStringLiteral("hash")
                                << static_cast<int>(appellate::packs::ErrorCode::DigestMismatch);
    QTest::newRow("wrong MIME") << QStringLiteral("mime")
                                << static_cast<int>(appellate::packs::ErrorCode::InvalidManifest);
    QTest::newRow("wrong size") << QStringLiteral("size")
                                << static_cast<int>(appellate::packs::ErrorCode::DigestMismatch);
    QTest::newRow("wrong signature")
        << QStringLiteral("signature")
        << static_cast<int>(appellate::packs::ErrorCode::InvalidManifest);
    QTest::newRow("missing trailer")
        << QStringLiteral("trailer")
        << static_cast<int>(appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsInvalidBlob() {
    QFETCH(QString, failure);
    QFETCH(int, expected_code);
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto content_path = QStringLiteral("judges/measured.json");
    const auto blob_path = QStringLiteral("objects/order.pdf");
    auto payload = validPdf();
    if (failure == QStringLiteral("signature")) {
        payload = QByteArray("not-a-pdf\n%%EOF\n");
    } else if (failure == QStringLiteral("trailer")) {
        payload = QByteArray("%PDF-1.7\nmissing trailer\n");
    }
    QVERIFY(writeBytes(pack.path(), content_path, judge));
    QVERIFY(writeBytes(pack.path(), blob_path, payload));
    auto blob = blobEntry(blob_path, payload);
    if (failure == QStringLiteral("hash")) {
        blob.insert(QStringLiteral("sha256"), QString(64, u'0'));
    } else if (failure == QStringLiteral("mime")) {
        blob.insert(QStringLiteral("media_type"), QStringLiteral("text/plain"));
    } else if (failure == QStringLiteral("size")) {
        blob.insert(QStringLiteral("byte_size"), payload.size() + 1);
    }
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(
            QJsonArray{contentEntry(QStringLiteral("example.judge.measured"), content_path, judge)},
            {}, {}, QStringLiteral("1.0.0"), QJsonArray{blob})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(static_cast<int>(result.error().code), expected_code);
}

void PackReaderTest::rejectsBlobSizeBudgets() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto content_path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), content_path, judge));
    const QJsonArray contents{
        contentEntry(QStringLiteral("example.judge.measured"), content_path, judge)};
    auto oversized = blobEntry(QStringLiteral("objects/oversized.pdf"), validPdf());
    oversized.insert(QStringLiteral("byte_size"), qint64{512} * 1024 * 1024 + 1);
    QVERIFY(
        writeJson(pack.path(), QStringLiteral("manifest.json"),
                  validManifest(contents, {}, {}, QStringLiteral("1.0.0"), QJsonArray{oversized})));
    auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);

    QJsonArray excessive_total;
    for (int index = 0; index < 7; ++index) {
        auto blob = blobEntry(QStringLiteral("objects/order-%1.pdf").arg(index), validPdf());
        blob.insert(QStringLiteral("byte_size"), qint64{512} * 1024 * 1024);
        excessive_total.push_back(blob);
    }
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(contents, {}, {}, QStringLiteral("1.0.0"), excessive_total)));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::ResourceTooLarge);
}

void PackReaderTest::rejectsUnlistedAndOrphanBlobs() {
    const auto judge = jsonBytes(validJudge());
    const auto pdf = validPdf();
    const auto content_path = QStringLiteral("judges/measured.json");
    const auto blob_path = QStringLiteral("objects/order.pdf");

    QTemporaryDir unlisted;
    QVERIFY(unlisted.isValid());
    QVERIFY(writeBytes(unlisted.path(), content_path, judge));
    QVERIFY(writeBytes(unlisted.path(), blob_path, pdf));
    QVERIFY(writeJson(unlisted.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{contentEntry(
                          QStringLiteral("example.judge.measured"), content_path, judge)})));
    auto result = appellate::packs::PackReader::readDirectory(unlisted.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UndeclaredFile);

    QTemporaryDir orphan;
    QVERIFY(orphan.isValid());
    QVERIFY(writeBytes(orphan.path(), content_path, judge));
    QVERIFY(writeBytes(orphan.path(), blob_path, pdf));
    QVERIFY(writeJson(
        orphan.path(), QStringLiteral("manifest.json"),
        validManifest(
            QJsonArray{contentEntry(QStringLiteral("example.judge.measured"), content_path, judge)},
            {}, {}, QStringLiteral("1.0.0"), QJsonArray{blobEntry(blob_path, pdf)})));
    result = appellate::packs::PackReader::readDirectory(orphan.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
}

void PackReaderTest::rejectsMissingAndMismatchedRecordBlobs() {
    QTemporaryDir missing;
    QVERIFY(missing.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), missing.path()));
    QFile manifest_file(QDir(missing.path()).filePath(QStringLiteral("manifest.json")));
    QVERIFY(manifest_file.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(manifest_file.readAll()).object();
    manifest_file.close();
    manifest.insert(QStringLiteral("blobs"), QJsonArray{});
    QVERIFY(
        QFile::remove(QDir(missing.path()).filePath(QStringLiteral("objects/final-order.pdf"))));
    QVERIFY(writeJson(missing.path(), QStringLiteral("manifest.json"), manifest));
    auto result = appellate::packs::PackReader::readDirectory(missing.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);

    QTemporaryDir mismatched;
    QVERIFY(mismatched.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), mismatched.path()));
    const auto record_path = QStringLiteral("resources/record.json");
    QFile record_file(QDir(mismatched.path()).filePath(record_path));
    QVERIFY(record_file.open(QIODevice::ReadOnly));
    auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    record_file.close();
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    auto entry = entries.at(0).toObject();
    entry.insert(QStringLiteral("asset_sha256"), QString(64, u'0'));
    entries.replace(0, entry);
    QVERIFY(replaceResourceField(mismatched.path(), record_path, QStringLiteral("docket_entries"),
                                 entries));
    result = appellate::packs::PackReader::readDirectory(mismatched.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
}

void PackReaderTest::acceptsLegacyRecordWithoutOptionalMetadata() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));

    const auto record_path = QStringLiteral("resources/record.json");
    QFile record_file(QDir(pack.path()).filePath(record_path));
    QVERIFY(record_file.open(QIODevice::ReadOnly));
    auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    record_file.close();
    record.remove(QStringLiteral("dockets"));
    record.remove(QStringLiteral("page_anchors"));
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    auto entry = entries.at(0).toObject();
    for (const auto& field :
         {QStringLiteral("docket_id"), QStringLiteral("entry_label"), QStringLiteral("actor"),
          QStringLiteral("description"), QStringLiteral("tags")}) {
        entry.remove(field);
    }
    entries.replace(0, entry);
    record.insert(QStringLiteral("docket_entries"), entries);
    QVERIFY(replaceResourceDocument(pack.path(), record_path, record));

    const auto case_path = QStringLiteral("resources/case.json");
    QFile case_file(QDir(pack.path()).filePath(case_path));
    QVERIFY(case_file.open(QIODevice::ReadOnly));
    auto case_resource = QJsonDocument::fromJson(case_file.readAll()).object();
    case_file.close();
    auto issues = case_resource.value(QStringLiteral("issues")).toArray();
    auto issue = issues.at(0).toObject();
    issue.insert(QStringLiteral("record_anchor_ids"),
                 QJsonArray{QStringLiteral("example.record.entry-one")});
    issues.replace(0, issue);
    case_resource.insert(QStringLiteral("issues"), issues);
    QVERIFY(replaceResourceDocument(pack.path(), case_path, case_resource));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
}

void PackReaderTest::rejectsInvalidRecordMetadataGraph() {
    const auto record_path = QStringLiteral("resources/record.json");
    for (int variant = 0; variant < 8; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
        QFile record_file(QDir(pack.path()).filePath(record_path));
        QVERIFY(record_file.open(QIODevice::ReadOnly));
        auto record = QJsonDocument::fromJson(record_file.readAll()).object();
        record_file.close();
        auto entries = record.value(QStringLiteral("docket_entries")).toArray();
        auto first = entries.at(0).toObject();
        auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
        if (variant == 0) {
            first.insert(QStringLiteral("docket_id"), QStringLiteral("example.docket.missing"));
            entries.replace(0, first);
            record.insert(QStringLiteral("docket_entries"), entries);
        } else if (variant == 1) {
            first.insert(QStringLiteral("parent_entry_id"),
                         QStringLiteral("example.record.entry-two"));
            entries.replace(0, first);
            record.insert(QStringLiteral("docket_entries"), entries);
        } else if (variant == 2) {
            first.insert(QStringLiteral("parent_entry_id"),
                         QStringLiteral("example.record.missing"));
            first.insert(QStringLiteral("relationship"), QStringLiteral("attachment"));
            entries.replace(0, first);
            record.insert(QStringLiteral("docket_entries"), entries);
        } else if (variant == 3) {
            auto second = first;
            second.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.entry-two"));
            second.insert(QStringLiteral("entry_number"), 2);
            second.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-1"));
            second.insert(QStringLiteral("parent_entry_id"),
                          QStringLiteral("example.record.entry-one"));
            second.insert(QStringLiteral("relationship"), QStringLiteral("attachment"));
            first.insert(QStringLiteral("parent_entry_id"),
                         QStringLiteral("example.record.entry-two"));
            first.insert(QStringLiteral("relationship"), QStringLiteral("component"));
            entries.replace(0, first);
            entries.push_back(second);
            record.insert(QStringLiteral("docket_entries"), entries);
        } else if (variant == 4) {
            auto anchor = anchors.at(0).toObject();
            anchor.insert(QStringLiteral("page_number"), 4);
            anchors.replace(0, anchor);
            record.insert(QStringLiteral("page_anchors"), anchors);
        } else if (variant == 5) {
            auto anchor = anchors.at(0).toObject();
            anchor.insert(QStringLiteral("anchor_id"), QStringLiteral("example.record.entry-one"));
            anchors.replace(0, anchor);
            record.insert(QStringLiteral("page_anchors"), anchors);
        } else if (variant == 6) {
            auto dockets = record.value(QStringLiteral("dockets")).toArray();
            auto appellate = dockets.at(1).toObject();
            appellate.insert(QStringLiteral("court_id"), QStringLiteral("example.court.missing"));
            dockets.replace(1, appellate);
            record.insert(QStringLiteral("dockets"), dockets);
        } else {
            auto duplicate_citation = anchors.at(0).toObject();
            duplicate_citation.insert(QStringLiteral("anchor_id"),
                                      QStringLiteral("example.record.anchor.ja3"));
            duplicate_citation.insert(QStringLiteral("page_number"), 3);
            anchors.push_back(duplicate_citation);
            record.insert(QStringLiteral("page_anchors"), anchors);
        }
        QVERIFY(replaceResourceDocument(pack.path(), record_path, record));
        const auto result = appellate::packs::PackReader::readDirectory(pack.path());
        QVERIFY2(!result.has_value(), qPrintable(QStringLiteral("variant %1").arg(variant)));
        QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir unknown_field;
    QVERIFY(unknown_field.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), unknown_field.path()));
    QFile record_file(QDir(unknown_field.path()).filePath(record_path));
    QVERIFY(record_file.open(QIODevice::ReadOnly));
    auto record = QJsonDocument::fromJson(record_file.readAll()).object();
    record_file.close();
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    auto entry = entries.at(0).toObject();
    entry.insert(QStringLiteral("remote_url"), QStringLiteral("https://example.invalid/order"));
    entries.replace(0, entry);
    record.insert(QStringLiteral("docket_entries"), entries);
    QVERIFY(replaceResourceDocument(unknown_field.path(), record_path, record));
    const auto result = appellate::packs::PackReader::readDirectory(unknown_field.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::SchemaViolation);
}

void PackReaderTest::validatesUnicodeScalarLength() {
    const auto record_path = QStringLiteral("resources/record.json");
    QTemporaryDir boundary;
    QVERIFY(boundary.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), boundary.path()));
    QFile boundary_file(QDir(boundary.path()).filePath(record_path));
    QVERIFY(boundary_file.open(QIODevice::ReadOnly));
    auto boundary_record = QJsonDocument::fromJson(boundary_file.readAll()).object();
    boundary_file.close();
    auto boundary_entries = boundary_record.value(QStringLiteral("docket_entries")).toArray();
    auto boundary_entry = boundary_entries.at(0).toObject();
    boundary_entry.insert(QStringLiteral("actor"), QString(240, QChar(0xD55C)));
    boundary_entries.replace(0, boundary_entry);
    boundary_record.insert(QStringLiteral("docket_entries"), boundary_entries);
    QVERIFY(replaceResourceDocument(boundary.path(), record_path, boundary_record));
    const auto boundary_result = appellate::packs::PackReader::readDirectory(boundary.path());
    QVERIFY2(boundary_result.has_value(),
             boundary_result ? "" : qPrintable(boundary_result.error().message));
    const auto boundary_runtime = appellate::packs::loadRuntimePack(*boundary_result);
    QVERIFY2(boundary_runtime.has_value(),
             boundary_runtime ? "" : boundary_runtime.error().message.c_str());
    QCOMPARE(*boundary_runtime->cases.front().record.docket_entries.front().actor,
             QString(240, QChar(0xD55C)).toUtf8().toStdString());

    QTemporaryDir overflow;
    QVERIFY(overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), overflow.path()));
    QFile overflow_file(QDir(overflow.path()).filePath(record_path));
    QVERIFY(overflow_file.open(QIODevice::ReadOnly));
    auto overflow_record = QJsonDocument::fromJson(overflow_file.readAll()).object();
    overflow_file.close();
    auto overflow_entries = overflow_record.value(QStringLiteral("docket_entries")).toArray();
    auto overflow_entry = overflow_entries.at(0).toObject();
    overflow_entry.insert(QStringLiteral("actor"), QString(241, QChar(0xD55C)));
    overflow_entries.replace(0, overflow_entry);
    overflow_record.insert(QStringLiteral("docket_entries"), overflow_entries);
    QVERIFY(replaceResourceDocument(overflow.path(), record_path, overflow_record));
    const auto overflow_result = appellate::packs::PackReader::readDirectory(overflow.path());
    QVERIFY(!overflow_result.has_value());
    QCOMPARE(overflow_result.error().code, appellate::packs::ErrorCode::SchemaViolation);
}

void PackReaderTest::digestIncludesBlobDescriptor() {
    QTemporaryDir original;
    QTemporaryDir changed;
    QVERIFY(original.isValid());
    QVERIFY(changed.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), original.path()));
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), changed.path()));
    QFile blob_file(QDir(changed.path()).filePath(QStringLiteral("objects/final-order.pdf")));
    QVERIFY(blob_file.open(QIODevice::ReadOnly));
    auto changed_payload = blob_file.readAll();
    blob_file.close();
    changed_payload.append(' ');
    QVERIFY(updateFixtureBlob(changed.path(), changed_payload));

    const auto original_result = appellate::packs::PackReader::readDirectory(original.path());
    const auto changed_result = appellate::packs::PackReader::readDirectory(changed.path());
    QVERIFY(original_result.has_value());
    QVERIFY(changed_result.has_value());
    QVERIFY(original_result->revision.digest != changed_result->revision.digest);
}

void PackReaderTest::rejectsMalformedJson() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("malformed-manifest")));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidJson);
}

void PackReaderTest::rejectsUnsupportedSchema() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("unsupported-schema")));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsupportedSchema);
}

void PackReaderTest::rejectsPathTraversal() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("path-traversal")));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsafePath);
}

void PackReaderTest::rejectsDuplicateContentId() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("duplicate-content")));
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentId);
}

void PackReaderTest::rejectsUnknownManifestField() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), judge));
    auto manifest = validManifest(QJsonArray{contentEntry(
        QStringLiteral("example.judge.measured"), QStringLiteral("judges/measured.json"), judge)});
    manifest.insert(QStringLiteral("script"), QStringLiteral("ignored-before-this-fix"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsProhibitedJudgeField() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    auto profile = validJudge();
    profile.insert(QStringLiteral("outcome_weight"), 1.0);
    const auto bytes = jsonBytes(profile);
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), bytes));
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                              QStringLiteral("judges/measured.json"), bytes)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidJudgeProfile);
}

void PackReaderTest::rejectsInvalidStructuredVoice() {
    const auto rejects = [](QJsonObject profile) {
        QTemporaryDir pack;
        if (!pack.isValid()) {
            return false;
        }
        const auto bytes = jsonBytes(profile);
        if (!writeBytes(pack.path(), QStringLiteral("judges/measured.json"), bytes) ||
            !writeJson(pack.path(), QStringLiteral("manifest.json"),
                       validManifest(QJsonArray{
                           contentEntry(QStringLiteral("example.judge.measured"),
                                        QStringLiteral("judges/measured.json"), bytes)}))) {
            return false;
        }
        return !appellate::packs::PackReader::readDirectory(pack.path()).has_value();
    };

    auto invalid_enum = validJudge();
    auto voice = invalid_enum.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("question_framing"), QStringLiteral("performative"));
    invalid_enum.insert(QStringLiteral("voice"), voice);
    QVERIFY(rejects(invalid_enum));

    auto duplicate = validJudge();
    voice = duplicate.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("question_phrases"),
                 QJsonArray{QStringLiteral("repeat"), QStringLiteral("repeat")});
    duplicate.insert(QStringLiteral("voice"), voice);
    QVERIFY(rejects(duplicate));

    auto template_injection = validJudge();
    voice = template_injection.value(QStringLiteral("voice")).toObject();
    voice.insert(QStringLiteral("clarification_phrases"),
                 QJsonArray{QStringLiteral("imitate {named_judge}")});
    template_injection.insert(QStringLiteral("voice"), voice);
    QVERIFY(rejects(template_injection));

    auto too_many = validJudge();
    voice = too_many.value(QStringLiteral("voice")).toObject();
    QJsonArray phrases;
    for (int index = 0; index < 9; ++index) {
        phrases.append(QStringLiteral("bounded phrase %1").arg(index));
    }
    voice.insert(QStringLiteral("interruption_phrases"), phrases);
    too_many.insert(QStringLiteral("voice"), voice);
    QVERIFY(rejects(too_many));
}

void PackReaderTest::rejectsInvalidIdentifiersVersionsAndHashes() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), judge));
    auto entry = contentEntry(QStringLiteral("example.judge.measured"),
                              QStringLiteral("judges/measured.json"), judge);
    auto manifest = validManifest(QJsonArray{entry});
    manifest.insert(QStringLiteral("pack_id"), QStringLiteral("Not Namespaced"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));
    auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);

    manifest = validManifest(QJsonArray{entry}, {}, {}, QStringLiteral("01.0.0"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"), manifest));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);

    entry.insert(QStringLiteral("sha256"), QString(64, u'A'));
    QVERIFY(
        writeJson(pack.path(), QStringLiteral("manifest.json"), validManifest(QJsonArray{entry})));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsMalformedCapabilityAndDependency() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), judge));
    const QJsonArray contents{contentEntry(QStringLiteral("example.judge.measured"),
                                           QStringLiteral("judges/measured.json"), judge)};
    auto bad_capability = capability(QStringLiteral("workbench.pack.judge-profile"));
    bad_capability.insert(QStringLiteral("optional"), true);
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(contents, QJsonArray{bad_capability})));
    auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);

    auto bad_dependency =
        dependency(QStringLiteral("example.base.pack"), QStringLiteral("2.0.0"), u'2');
    bad_dependency.remove(QStringLiteral("sha256"));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(contents, {}, QJsonArray{bad_dependency})));
    result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::InvalidManifest);
}

void PackReaderTest::rejectsDuplicateContentPath() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), path, judge));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{
                          contentEntry(QStringLiteral("example.judge.measured"), path, judge),
                          contentEntry(QStringLiteral("example.judge.second"), path, judge),
                      })));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateContentPath);
}

void PackReaderTest::rejectsDuplicatePayloadId() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/first.json"), judge));
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/second.json"), judge));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{
                          contentEntry(QStringLiteral("example.judge.measured"),
                                       QStringLiteral("judges/first.json"), judge),
                          contentEntry(QStringLiteral("example.judge.second"),
                                       QStringLiteral("judges/second.json"), judge),
                      })));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicatePayloadId);
}

void PackReaderTest::rejectsUndeclaredFile() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/measured.json"), judge));
    QVERIFY(writeBytes(pack.path(), QStringLiteral("payload.exe"), QByteArray("not executable")));
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                              QStringLiteral("judges/measured.json"), judge)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UndeclaredFile);
}

void PackReaderTest::rejectsIntermediateSymlink() {
    QTemporaryDir pack;
    QTemporaryDir outside;
    QVERIFY(pack.isValid());
    QVERIFY(outside.isValid());
    const auto judge = jsonBytes(validJudge());
    QVERIFY(writeBytes(outside.path(), QStringLiteral("measured.json"), judge));
    if (!QFile::link(outside.path(), QDir(pack.path()).filePath(QStringLiteral("judges")))) {
        QSKIP("This platform cannot create a directory symlink in the test environment");
    }
    QVERIFY(writeJson(
        pack.path(), QStringLiteral("manifest.json"),
        validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                              QStringLiteral("judges/measured.json"), judge)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsafePath);
}

void PackReaderTest::rejectsOversizedJson() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const QByteArray oversized(8 * 1024 * 1024 + 1, '{');
    QVERIFY(writeBytes(pack.path(), QStringLiteral("judges/oversized.json"), oversized));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{
                          contentEntry(QStringLiteral("example.judge.oversized"),
                                       QStringLiteral("judges/oversized.json"), oversized)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::ResourceTooLarge);
}

void PackReaderTest::producesCanonicalOrderIndependentDigest() {
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    const auto measured = jsonBytes(validJudge());
    const auto concise = jsonBytes(
        validJudge(QStringLiteral("example.judge.concise"), QStringLiteral("Concise Panelist")));
    for (const auto& root : {first.path(), second.path()}) {
        QVERIFY(writeBytes(root, QStringLiteral("judges/measured.json"), measured));
        QVERIFY(writeBytes(root, QStringLiteral("judges/concise.json"), concise));
    }
    const auto measured_entry = contentEntry(QStringLiteral("example.judge.measured"),
                                             QStringLiteral("judges/measured.json"), measured);
    const auto concise_entry = contentEntry(QStringLiteral("example.judge.concise"),
                                            QStringLiteral("judges/concise.json"), concise);
    const auto capability_one = capability(QStringLiteral("workbench.pack.judge-profile"));
    const auto capability_two = capability(QStringLiteral("workbench.pack.voice-style"));
    const auto dependency_one =
        dependency(QStringLiteral("example.base.one"), QStringLiteral("1.0.0"), u'1');
    const auto dependency_two =
        dependency(QStringLiteral("example.base.two"), QStringLiteral("2.0.0"), u'2');
    QVERIFY(writeJson(first.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{measured_entry, concise_entry},
                                    QJsonArray{capability_one, capability_two},
                                    QJsonArray{dependency_one, dependency_two})));
    QVERIFY(writeJson(second.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{concise_entry, measured_entry},
                                    QJsonArray{capability_two, capability_one},
                                    QJsonArray{dependency_two, dependency_one})));

    const auto first_result = appellate::packs::PackReader::readDirectory(first.path());
    const auto second_result = appellate::packs::PackReader::readDirectory(second.path());
    QVERIFY(first_result.has_value());
    QVERIFY(second_result.has_value());
    QCOMPARE(first_result->revision.digest, second_result->revision.digest);
    QCOMPARE(first_result->judge_profiles.front().id, std::string("example.judge.concise"));
}

void PackReaderTest::digestIncludesPathAndDependencies() {
    QTemporaryDir first;
    QTemporaryDir changed_path;
    QTemporaryDir changed_dependency;
    QVERIFY(first.isValid());
    QVERIFY(changed_path.isValid());
    QVERIFY(changed_dependency.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto original_path = QStringLiteral("judges/measured.json");
    const auto alternate_path = QStringLiteral("profiles/measured.json");
    QVERIFY(writeBytes(first.path(), original_path, judge));
    QVERIFY(writeBytes(changed_path.path(), alternate_path, judge));
    QVERIFY(writeBytes(changed_dependency.path(), original_path, judge));
    const auto first_dependency =
        dependency(QStringLiteral("example.base.pack"), QStringLiteral("1.0.0"), u'1');
    const auto second_dependency =
        dependency(QStringLiteral("example.base.pack"), QStringLiteral("1.0.0"), u'2');
    QVERIFY(
        writeJson(first.path(), QStringLiteral("manifest.json"),
                  validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                                        original_path, judge)},
                                {}, QJsonArray{first_dependency})));
    QVERIFY(
        writeJson(changed_path.path(), QStringLiteral("manifest.json"),
                  validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                                        alternate_path, judge)},
                                {}, QJsonArray{first_dependency})));
    QVERIFY(
        writeJson(changed_dependency.path(), QStringLiteral("manifest.json"),
                  validManifest(QJsonArray{contentEntry(QStringLiteral("example.judge.measured"),
                                                        original_path, judge)},
                                {}, QJsonArray{second_dependency})));

    const auto first_result = appellate::packs::PackReader::readDirectory(first.path());
    const auto path_result = appellate::packs::PackReader::readDirectory(changed_path.path());
    const auto dependency_result =
        appellate::packs::PackReader::readDirectory(changed_dependency.path());
    QVERIFY(first_result.has_value());
    QVERIFY(path_result.has_value());
    QVERIFY(dependency_result.has_value());
    QVERIFY(first_result->revision.digest != path_result->revision.digest);
    QVERIFY(first_result->revision.digest != dependency_result->revision.digest);
}

void PackReaderTest::rejectsDuplicateJsonKeys() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    auto judge = jsonBytes(validJudge());
    const QByteArray identity("\"resource_id\":\"example.judge.measured\"");
    QVERIFY(judge.contains(identity));
    judge.replace(identity, identity + QByteArray(",") + identity);
    const auto path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), path, judge));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{
                          contentEntry(QStringLiteral("example.judge.measured"), path, judge)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::DuplicateJsonKey);
}

void PackReaderTest::rejectsUnknownResourceKind() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), path, judge));
    auto entry = contentEntry(QStringLiteral("example.judge.measured"), path, judge);
    entry.insert(QStringLiteral("kind"), QStringLiteral("native_plugin"));
    QVERIFY(
        writeJson(pack.path(), QStringLiteral("manifest.json"), validManifest(QJsonArray{entry})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsupportedResourceKind);
}

void PackReaderTest::rejectsUnsupportedResourceSchema() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), path, judge));
    auto entry = contentEntry(QStringLiteral("example.judge.measured"), path, judge);
    entry.insert(QStringLiteral("schema_version"), 2);
    QVERIFY(
        writeJson(pack.path(), QStringLiteral("manifest.json"), validManifest(QJsonArray{entry})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::UnsupportedSchema);
}

void PackReaderTest::rejectsGenericSchemaViolation() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
    QVERIFY(replaceResourceField(pack.path(), QStringLiteral("resources/court.json"),
                                 QStringLiteral("native_hook"), QStringLiteral("not allowed")));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::SchemaViolation);
}

void PackReaderTest::rejectsDescriptorPayloadDisagreement() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    const auto judge = jsonBytes(validJudge());
    const auto path = QStringLiteral("judges/measured.json");
    QVERIFY(writeBytes(pack.path(), path, judge));
    QVERIFY(writeJson(pack.path(), QStringLiteral("manifest.json"),
                      validManifest(QJsonArray{
                          contentEntry(QStringLiteral("example.judge.different"), path, judge)})));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::SchemaViolation);
}

void PackReaderTest::rejectsBrokenCrossReference() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
    QVERIFY(replaceResourceField(pack.path(), QStringLiteral("resources/court.json"),
                                 QStringLiteral("authority_set_ids"),
                                 QJsonArray{QStringLiteral("example.authorities.missing")}));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
}

void PackReaderTest::rejectsIncompleteWorkflowAuthority() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
    const auto relative_path = QStringLiteral("resources/workflow.json");
    QFile workflow_file(QDir(pack.path()).filePath(relative_path));
    QVERIFY(workflow_file.open(QIODevice::ReadOnly));
    const auto workflow_document = QJsonDocument::fromJson(workflow_file.readAll()).object();
    auto operations = workflow_document.value(QStringLiteral("operations")).toArray();
    auto operation = operations.at(0).toObject();
    auto authority = operation.value(QStringLiteral("authority")).toObject();
    auto primary = authority.value(QStringLiteral("primary")).toObject();
    primary.remove(QStringLiteral("source_version"));
    authority.insert(QStringLiteral("primary"), primary);
    operation.insert(QStringLiteral("authority"), authority);
    operations.replace(0, operation);
    QVERIFY(
        replaceResourceField(pack.path(), relative_path, QStringLiteral("operations"), operations));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::SchemaViolation);
}

void PackReaderTest::rejectsConflictingWorkflowAuthority() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
    const auto relative_path = QStringLiteral("resources/workflow.json");
    QFile workflow_file(QDir(pack.path()).filePath(relative_path));
    QVERIFY(workflow_file.open(QIODevice::ReadOnly));
    const auto workflow_document = QJsonDocument::fromJson(workflow_file.readAll()).object();
    auto operations = workflow_document.value(QStringLiteral("operations")).toArray();
    auto operation = operations.at(0).toObject();
    auto authority = operation.value(QStringLiteral("authority")).toObject();
    auto primary = authority.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("proposition"), QStringLiteral("Fabricated proposition"));
    authority.insert(QStringLiteral("primary"), primary);
    operation.insert(QStringLiteral("authority"), authority);
    operations.replace(0, operation);
    QVERIFY(
        replaceResourceField(pack.path(), relative_path, QStringLiteral("operations"), operations));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
}

void PackReaderTest::acceptsCatalogSupersetAndStagesWithoutFallbackRejections() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));

    const auto catalog_path = QStringLiteral("resources/filing-catalog.json");
    QFile catalog_file(QDir(pack.path()).filePath(catalog_path));
    QVERIFY(catalog_file.open(QIODevice::ReadOnly));
    const auto catalog_document = QJsonDocument::fromJson(catalog_file.readAll()).object();
    auto filings = catalog_document.value(QStringLiteral("filings")).toArray();
    filings.push_back(QJsonObject{
        {QStringLiteral("filing_id"), QStringLiteral("example.filing.reference-template")},
        {QStringLiteral("title"), QStringLiteral("Reference-only filing template")},
        {QStringLiteral("actor_role_ids"), QJsonArray{QStringLiteral("example.role.appellant")}},
        {QStringLiteral("required_field_ids"), QJsonArray{}},
        {QStringLiteral("authority_id"), QStringLiteral("example.authority.rule-one")},
    });
    QVERIFY(replaceResourceField(pack.path(), catalog_path, QStringLiteral("filings"), filings));

    const auto workflow_path = QStringLiteral("resources/workflow.json");
    QFile workflow_file(QDir(pack.path()).filePath(workflow_path));
    QVERIFY(workflow_file.open(QIODevice::ReadOnly));
    const auto workflow_document = QJsonDocument::fromJson(workflow_file.readAll()).object();
    const auto operations = workflow_document.value(QStringLiteral("operations")).toArray();
    QJsonArray without_unrouted_stage_rejection;
    for (const auto& value : operations) {
        if (value.toObject().value(QStringLiteral("operation_id")).toString() !=
            QStringLiteral("example.operation.reject-submitted")) {
            without_unrouted_stage_rejection.push_back(value);
        }
    }
    QVERIFY(replaceResourceField(pack.path(), workflow_path, QStringLiteral("operations"),
                                 without_unrouted_stage_rejection));

    const auto result = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
}

void PackReaderTest::rejectsWorkflowInvariantViolation() {
    const auto relative_path = QStringLiteral("resources/workflow.json");
    for (int variant = 0; variant < 3; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), pack.path()));
        QFile workflow_file(QDir(pack.path()).filePath(relative_path));
        QVERIFY(workflow_file.open(QIODevice::ReadOnly));
        const auto workflow_document = QJsonDocument::fromJson(workflow_file.readAll()).object();
        auto operations = workflow_document.value(QStringLiteral("operations")).toArray();
        auto routes = workflow_document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        if (variant == 0) {
            QJsonArray without_route_rejection;
            for (const auto& value : operations) {
                if (value.toObject().value(QStringLiteral("operation_id")).toString() !=
                    QStringLiteral("example.operation.reject-opened")) {
                    without_route_rejection.push_back(value);
                }
            }
            operations = without_route_rejection;
            QVERIFY(replaceResourceField(pack.path(), relative_path, QStringLiteral("operations"),
                                         operations));
        } else if (variant == 1) {
            route.insert(QStringLiteral("reject_operation_id"),
                         QStringLiteral("example.operation.accept-notice"));
            routes.replace(0, route);
            QVERIFY(replaceResourceField(pack.path(), relative_path,
                                         QStringLiteral("filing_routes"), routes));
        } else {
            route.remove(QStringLiteral("deficiency_operation_id"));
            routes.replace(0, route);
            QVERIFY(replaceResourceField(pack.path(), relative_path,
                                         QStringLiteral("filing_routes"), routes));
        }

        const auto result = appellate::packs::PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
    }
}

void PackReaderTest::loadsWorkflowCapabilitySliceAndFencesCoverage() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(configureWorkflowCapabilities(pack.path()));

    const auto loaded = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& workflow = runtime->cases.front().workflow;
    QCOMPARE(workflow.filing_routes.front().authorized_role_scope,
             appellate::model::WorkflowAuthorizedRoleScope::CatalogSubset);
    QVERIFY(workflow.filing_routes.front().deficiency_deadline->static_trigger.has_value());
    QVERIFY(std::ranges::any_of(workflow.operations, [](const auto& operation) {
        return operation.document_binding.has_value();
    }));
    const auto bound_judgment =
        std::ranges::find(workflow.operations,
                          appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
                          &appellate::model::WorkflowOperation::id);
    QVERIFY(bound_judgment != workflow.operations.end());
    QCOMPARE(bound_judgment->disposition_plan_id,
             std::optional{appellate::model::DispositionPlanId{"example.disposition.fictional"}});
    QVERIFY(std::ranges::any_of(workflow.operations, [](const auto& operation) {
        return std::ranges::any_of(operation.preconditions, [](const auto& precondition) {
            return std::holds_alternative<appellate::model::WorkflowFilingInstancePrecondition>(
                       precondition) ||
                   std::holds_alternative<appellate::model::WorkflowOrderInstancePrecondition>(
                       precondition);
        });
    }));

    const QStringList capability_ids{
        QStringLiteral("workbench.pack.structured-disposition"),
        QStringLiteral("workbench.pack.route-role-subsets"),
        QStringLiteral("workbench.pack.workflow-instance-preconditions"),
        QStringLiteral("workbench.pack.static-deficiency-deadlines"),
        QStringLiteral("workbench.pack.operation-document-bindings"),
        QStringLiteral("workbench.pack.operation-disposition-bindings")};
    for (const auto& capability_id : capability_ids) {
        QTemporaryDir missing;
        QVERIFY(missing.isValid());
        QVERIFY(copyTree(pack.path(), missing.path()));
        QVERIFY(setCapability(missing.path(), capability_id, std::nullopt));
        const auto rejected = appellate::packs::PackReader::readDirectory(missing.path());
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, appellate::packs::ErrorCode::UnsupportedCapability);

        auto forged = *loaded;
        std::erase_if(forged.required_capabilities, [&](const auto& capability) {
            return QString::fromStdString(capability.id) == capability_id;
        });
        const auto runtime_rejected = appellate::packs::loadRuntimePack(forged);
        QVERIFY(!runtime_rejected.has_value());
        QCOMPARE(runtime_rejected.error().code,
                 appellate::packs::RuntimePackErrorCode::InvalidPack);
    }

    QTemporaryDir unknown_version;
    QVERIFY(unknown_version.isValid());
    QVERIFY(copyTree(pack.path(), unknown_version.path()));
    QVERIFY(setCapability(unknown_version.path(), capability_ids.front(), 2));
    const auto rejected_version =
        appellate::packs::PackReader::readDirectory(unknown_version.path());
    QVERIFY(!rejected_version.has_value());
    QCOMPARE(rejected_version.error().code, appellate::packs::ErrorCode::UnsupportedCapability);

    QTemporaryDir subset;
    QVERIFY(subset.isValid());
    QVERIFY(copyTree(pack.path(), subset.path()));
    const auto catalog_path = QStringLiteral("resources/filing-catalog.json");
    QFile catalog_file(QDir(subset.path()).filePath(catalog_path));
    QVERIFY(catalog_file.open(QIODevice::ReadOnly));
    auto catalog = QJsonDocument::fromJson(catalog_file.readAll()).object();
    catalog_file.close();
    auto filings = catalog.value(QStringLiteral("filings")).toArray();
    auto catalog_filing = filings.at(0).toObject();
    catalog_filing.insert(QStringLiteral("actor_role_ids"),
                          QJsonArray{QStringLiteral("example.role.appellant"),
                                     QStringLiteral("example.role.appellee")});
    filings.replace(0, catalog_filing);
    catalog.insert(QStringLiteral("filings"), filings);
    QVERIFY(replaceResourceDocument(subset.path(), catalog_path, catalog));
    const auto valid_subset = appellate::packs::PackReader::readDirectory(subset.path());
    QVERIFY2(valid_subset.has_value(),
             valid_subset ? "" : qPrintable(valid_subset.error().message));

    const auto workflow_path = QStringLiteral("resources/workflow.json");
    QFile workflow_file(QDir(subset.path()).filePath(workflow_path));
    QVERIFY(workflow_file.open(QIODevice::ReadOnly));
    auto workflow_document = QJsonDocument::fromJson(workflow_file.readAll()).object();
    workflow_file.close();
    auto routes = workflow_document.value(QStringLiteral("filing_routes")).toArray();
    auto exact_route = routes.at(0).toObject();
    exact_route.remove(QStringLiteral("authorized_role_scope"));
    routes.replace(0, exact_route);
    workflow_document.insert(QStringLiteral("filing_routes"), routes);
    QVERIFY(replaceResourceDocument(subset.path(), workflow_path, workflow_document));
    const auto rejected_exact = appellate::packs::PackReader::readDirectory(subset.path());
    QVERIFY(!rejected_exact.has_value());
    QCOMPARE(rejected_exact.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);

    QTemporaryDir legacy;
    QVERIFY(legacy.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), legacy.path()));
    const auto legacy_workflow_path = QStringLiteral("resources/workflow.json");
    QFile legacy_workflow_file(QDir(legacy.path()).filePath(legacy_workflow_path));
    QVERIFY(legacy_workflow_file.open(QIODevice::ReadOnly));
    auto legacy_workflow = QJsonDocument::fromJson(legacy_workflow_file.readAll()).object();
    legacy_workflow_file.close();
    auto legacy_operations = legacy_workflow.value(QStringLiteral("operations")).toArray();
    auto legacy_judgment = legacy_operations.at(legacy_operations.size() - 1).toObject();
    legacy_judgment.insert(QStringLiteral("disposition_plan_id"),
                           QStringLiteral("example.disposition.fictional"));
    legacy_operations.replace(legacy_operations.size() - 1, legacy_judgment);
    legacy_workflow.insert(QStringLiteral("operations"), legacy_operations);
    QVERIFY(replaceResourceDocument(legacy.path(), legacy_workflow_path, legacy_workflow));
    const auto rejected_legacy = appellate::packs::PackReader::readDirectory(legacy.path());
    QVERIFY(!rejected_legacy.has_value());
    QCOMPARE(rejected_legacy.error().code, appellate::packs::ErrorCode::SchemaViolation);

    QTemporaryDir legacy_declaration;
    QVERIFY(legacy_declaration.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), legacy_declaration.path()));
    QVERIFY(setCapability(legacy_declaration.path(),
                          QStringLiteral("workbench.pack.operation-disposition-bindings"), 1));
    const auto rejected_legacy_declaration =
        appellate::packs::PackReader::readDirectory(legacy_declaration.path());
    QVERIFY(!rejected_legacy_declaration.has_value());
    QCOMPARE(rejected_legacy_declaration.error().code,
             appellate::packs::ErrorCode::UnsupportedCapability);

    QTemporaryDir malformed_binding;
    QVERIFY(malformed_binding.isValid());
    QVERIFY(copyTree(pack.path(), malformed_binding.path()));
    QFile malformed_workflow_file(QDir(malformed_binding.path()).filePath(workflow_path));
    QVERIFY(malformed_workflow_file.open(QIODevice::ReadOnly));
    auto malformed_workflow = QJsonDocument::fromJson(malformed_workflow_file.readAll()).object();
    malformed_workflow_file.close();
    auto malformed_operations = malformed_workflow.value(QStringLiteral("operations")).toArray();
    for (qsizetype index = 0; index < malformed_operations.size(); ++index) {
        auto operation = malformed_operations.at(index).toObject();
        if (operation.value(QStringLiteral("operation_id")).toString() !=
            QStringLiteral("example.operation.issue-judgment")) {
            continue;
        }
        operation.insert(QStringLiteral("disposition_plan_id"), 7);
        malformed_operations.replace(index, operation);
    }
    malformed_workflow.insert(QStringLiteral("operations"), malformed_operations);
    QVERIFY(replaceResourceDocument(malformed_binding.path(), workflow_path, malformed_workflow));
    const auto rejected_malformed =
        appellate::packs::PackReader::readDirectory(malformed_binding.path());
    QVERIFY(!rejected_malformed.has_value());
    QCOMPARE(rejected_malformed.error().code, appellate::packs::ErrorCode::SchemaViolation);

    QTemporaryDir wrong_opcode_binding;
    QVERIFY(wrong_opcode_binding.isValid());
    QVERIFY(copyTree(pack.path(), wrong_opcode_binding.path()));
    QFile wrong_opcode_file(QDir(wrong_opcode_binding.path()).filePath(workflow_path));
    QVERIFY(wrong_opcode_file.open(QIODevice::ReadOnly));
    auto wrong_opcode_workflow = QJsonDocument::fromJson(wrong_opcode_file.readAll()).object();
    wrong_opcode_file.close();
    auto wrong_opcode_operations =
        wrong_opcode_workflow.value(QStringLiteral("operations")).toArray();
    auto wrong_opcode_operation = wrong_opcode_operations.at(0).toObject();
    wrong_opcode_operation.insert(QStringLiteral("disposition_plan_id"),
                                  QStringLiteral("example.disposition.fictional"));
    wrong_opcode_operations.replace(0, wrong_opcode_operation);
    wrong_opcode_workflow.insert(QStringLiteral("operations"), wrong_opcode_operations);
    QVERIFY(
        replaceResourceDocument(wrong_opcode_binding.path(), workflow_path, wrong_opcode_workflow));
    const auto rejected_wrong_opcode =
        appellate::packs::PackReader::readDirectory(wrong_opcode_binding.path());
    QVERIFY(!rejected_wrong_opcode.has_value());
    QCOMPARE(rejected_wrong_opcode.error().code,
             appellate::packs::ErrorCode::CrossReferenceFailure);

    QTemporaryDir orphan_binding;
    QVERIFY(orphan_binding.isValid());
    QVERIFY(copyTree(pack.path(), orphan_binding.path()));
    QFile orphan_source(QDir(orphan_binding.path()).filePath(workflow_path));
    QVERIFY(orphan_source.open(QIODevice::ReadOnly));
    auto orphan_workflow = QJsonDocument::fromJson(orphan_source.readAll()).object();
    orphan_source.close();
    const auto orphan_id = QStringLiteral("example.workflow.orphan-bound");
    const auto orphan_path = QStringLiteral("resources/workflow-orphan-bound.json");
    orphan_workflow.insert(QStringLiteral("resource_id"), orphan_id);
    const auto orphan_bytes = jsonBytes(orphan_workflow);
    QVERIFY(writeBytes(orphan_binding.path(), orphan_path, orphan_bytes));
    QFile orphan_manifest_file(
        QDir(orphan_binding.path()).filePath(QStringLiteral("manifest.json")));
    QVERIFY(orphan_manifest_file.open(QIODevice::ReadOnly));
    auto orphan_manifest = QJsonDocument::fromJson(orphan_manifest_file.readAll()).object();
    orphan_manifest_file.close();
    auto orphan_contents = orphan_manifest.value(QStringLiteral("contents")).toArray();
    orphan_contents.push_back(QJsonObject{{QStringLiteral("id"), orphan_id},
                                          {QStringLiteral("kind"), QStringLiteral("workflow")},
                                          {QStringLiteral("schema_version"), 2},
                                          {QStringLiteral("path"), orphan_path},
                                          {QStringLiteral("sha256"), sha256(orphan_bytes)}});
    orphan_manifest.insert(QStringLiteral("contents"), orphan_contents);
    QVERIFY(writeJson(orphan_binding.path(), QStringLiteral("manifest.json"), orphan_manifest));
    const auto rejected_orphan = appellate::packs::PackReader::readDirectory(orphan_binding.path());
    QVERIFY(!rejected_orphan.has_value());
    QCOMPARE(rejected_orphan.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
}

void PackReaderTest::rejectsForgedWorkflowCapabilityGraphs() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(configureWorkflowCapabilities(pack.path()));
    const auto loaded = appellate::packs::PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));

    const auto graph_rejects = [&](auto mutate) {
        auto forged = *loaded;
        mutate(forged);
        const auto checked = appellate::packs::PackReader::validateResolvedGraph(
            forged, std::span<const appellate::packs::LoadedPack* const>{});
        return !checked.has_value() &&
               checked.error().code == appellate::packs::ErrorCode::CrossReferenceFailure;
    };
    const auto workflow_resource = [](auto& forged) -> auto& {
        const auto found = std::ranges::find_if(forged.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
        });
        Q_ASSERT(found != forged.resources.end());
        return *found;
    };
    const auto record_resource = [](auto& forged) -> auto& {
        const auto found = std::ranges::find_if(forged.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
        Q_ASSERT(found != forged.resources.end());
        return *found;
    };
    const auto case_resource = [](auto& forged) -> auto& {
        const auto found = std::ranges::find_if(forged.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Case;
        });
        Q_ASSERT(found != forged.resources.end());
        return *found;
    };
    const auto add_orphan_bound_workflow = [&](auto& forged) {
        auto orphan = workflow_resource(forged);
        orphan.descriptor.id = "example.workflow.orphan-bound";
        orphan.descriptor.path = "resources/workflow-orphan-bound.json";
        orphan.document.insert(QStringLiteral("resource_id"),
                               QStringLiteral("example.workflow.orphan-bound"));
        forged.resources.push_back(std::move(orphan));
    };
    QVERIFY(graph_rejects(add_orphan_bound_workflow));
    auto orphan_bound_workflow = *loaded;
    add_orphan_bound_workflow(orphan_bound_workflow);
    const auto orphan_runtime = appellate::packs::loadRuntimePack(orphan_bound_workflow);
    QVERIFY(!orphan_runtime.has_value());
    QCOMPARE(orphan_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    const auto add_incompatible_workflow_owner = [&](auto& forged) {
        auto second_case = case_resource(forged);
        second_case.descriptor.id = "example.case.incompatible-owner";
        second_case.descriptor.path = "resources/case-incompatible-owner.json";
        second_case.document.insert(QStringLiteral("resource_id"),
                                    QStringLiteral("example.case.incompatible-owner"));
        auto issues = second_case.document.value(QStringLiteral("issues")).toArray();
        for (qsizetype index = 0; index < issues.size(); ++index) {
            auto issue = issues.at(index).toObject();
            issue.remove(QStringLiteral("target_ids"));
            issues.replace(index, issue);
        }
        second_case.document.insert(QStringLiteral("issues"), issues);
        second_case.document.remove(QStringLiteral("disposition_plans"));
        second_case.document.remove(QStringLiteral("authored_disposition_plan_id"));
        forged.resources.push_back(std::move(second_case));
    };
    QVERIFY(graph_rejects(add_incompatible_workflow_owner));
    auto incompatible_workflow_owner = *loaded;
    add_incompatible_workflow_owner(incompatible_workflow_owner);
    const auto incompatible_owner_runtime =
        appellate::packs::loadRuntimePack(incompatible_workflow_owner);
    QVERIFY(!incompatible_owner_runtime.has_value());
    QCOMPARE(incompatible_owner_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
    const auto filing_guard_rejects = [&](const QString& key, const QJsonValue& value) {
        return graph_rejects([&](auto& forged) {
            auto& resource = workflow_resource(forged);
            auto operations = resource.document.value(QStringLiteral("operations")).toArray();
            for (qsizetype index = 0; index < operations.size(); ++index) {
                auto operation = operations.at(index).toObject();
                if (operation.value(QStringLiteral("operation_id")).toString() !=
                    QStringLiteral("example.operation.issue-judgment")) {
                    continue;
                }
                auto guards = operation.value(QStringLiteral("preconditions")).toArray();
                auto guard = guards.at(1).toObject();
                guard.insert(key, value);
                guards.replace(1, guard);
                operation.insert(QStringLiteral("preconditions"), guards);
                operations.replace(index, operation);
            }
            resource.document.insert(QStringLiteral("operations"), operations);
        });
    };
    const auto static_trigger_rejects = [&](const QString& key, const QJsonValue& value) {
        return graph_rejects([&](auto& forged) {
            auto& resource = workflow_resource(forged);
            auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
            auto route = routes.at(0).toObject();
            auto plan = route.value(QStringLiteral("deficiency_deadline")).toObject();
            auto trigger = plan.value(QStringLiteral("trigger_filing")).toObject();
            trigger.insert(key, value);
            plan.insert(QStringLiteral("trigger_filing"), trigger);
            route.insert(QStringLiteral("deficiency_deadline"), plan);
            routes.replace(0, route);
            resource.document.insert(QStringLiteral("filing_routes"), routes);
        });
    };

    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        route.insert(QStringLiteral("authorized_role_scope"), QStringLiteral("unknown"));
        routes.replace(0, route);
        resource.document.insert(QStringLiteral("filing_routes"), routes);
    }));
    for (int variant = 0; variant < 3; ++variant) {
        QVERIFY(graph_rejects([&](auto& forged) {
            auto& resource = workflow_resource(forged);
            auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
            auto route = routes.at(0).toObject();
            if (variant == 0) {
                route.insert(QStringLiteral("authorized_role_ids"), QJsonArray{});
            } else if (variant == 1) {
                route.insert(QStringLiteral("authorized_role_ids"),
                             QJsonArray{QStringLiteral("example.role.appellee")});
            } else {
                route.insert(QStringLiteral("required_field_ids"), QJsonArray{});
            }
            routes.replace(0, route);
            resource.document.insert(QStringLiteral("filing_routes"), routes);
        }));
    }
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        auto plan = route.value(QStringLiteral("deficiency_deadline")).toObject();
        plan.insert(QStringLiteral("id_mode"), QStringLiteral("dynamic"));
        route.insert(QStringLiteral("deficiency_deadline"), plan);
        routes.replace(0, route);
        resource.document.insert(QStringLiteral("filing_routes"), routes);
    }));
    const auto add_invalid_accepted_deadline = [&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        route.insert(QStringLiteral("accepted_deadline"),
                     QJsonObject{{QStringLiteral("deadline_id"),
                                  QStringLiteral("example.deadline.invalid-accepted")},
                                 {QStringLiteral("operation_id"),
                                  QStringLiteral("example.operation.calculate-cure")},
                                 {QStringLiteral("id_mode"), QStringLiteral("exact")}});
        routes.replace(0, route);
        resource.document.insert(QStringLiteral("filing_routes"), routes);
    };
    QVERIFY(graph_rejects(add_invalid_accepted_deadline));
    auto forged_accepted_deadline = *loaded;
    add_invalid_accepted_deadline(forged_accepted_deadline);
    const auto rejected_accepted_deadline =
        appellate::packs::loadRuntimePack(forged_accepted_deadline);
    QVERIFY(!rejected_accepted_deadline.has_value());
    QCOMPARE(rejected_accepted_deadline.error().code,
             appellate::packs::RuntimePackErrorCode::InvalidResource);
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        auto plan = route.value(QStringLiteral("deficiency_deadline")).toObject();
        plan.remove(QStringLiteral("id_mode"));
        route.insert(QStringLiteral("deficiency_deadline"), plan);
        routes.replace(0, route);
        resource.document.insert(QStringLiteral("filing_routes"), routes);
    }));
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.at(0).toObject();
        auto plan = route.value(QStringLiteral("deficiency_deadline")).toObject();
        auto trigger = plan.value(QStringLiteral("trigger_filing")).toObject();
        trigger.insert(QStringLiteral("unexpected"), true);
        plan.insert(QStringLiteral("trigger_filing"), trigger);
        route.insert(QStringLiteral("deficiency_deadline"), plan);
        routes.replace(0, route);
        resource.document.insert(QStringLiteral("filing_routes"), routes);
    }));
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.enter-bound-order")) {
                continue;
            }
            auto binding = operation.value(QStringLiteral("document_binding")).toObject();
            binding.remove(QStringLiteral("disposition"));
            operation.insert(QStringLiteral("document_binding"), binding);
            operations.replace(index, operation);
        }
        resource.document.insert(QStringLiteral("operations"), operations);
    }));
    QVERIFY(filing_guard_rejects(QStringLiteral("record_entry_id"),
                                 QStringLiteral("example.record.missing")));
    QVERIFY(filing_guard_rejects(
        QStringLiteral("document_sha256"),
        QStringLiteral("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")));
    QVERIFY(filing_guard_rejects(QStringLiteral("accept_operation_id"),
                                 QStringLiteral("example.operation.issue-judgment")));
    QVERIFY(static_trigger_rejects(QStringLiteral("actor_id"),
                                   QStringLiteral("example.actor.appellee")));
    QVERIFY(static_trigger_rejects(
        QStringLiteral("document_sha256"),
        QStringLiteral("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")));
    QVERIFY(static_trigger_rejects(QStringLiteral("record_entry_id"),
                                   QStringLiteral("example.record.missing")));
    QVERIFY(static_trigger_rejects(QStringLiteral("expected_court_date"),
                                   QStringLiteral("2026-01-04")));
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.issue-judgment")) {
                continue;
            }
            auto guards = operation.value(QStringLiteral("preconditions")).toArray();
            auto order_guard = guards.at(2).toObject();
            order_guard.insert(QStringLiteral("operation_id"),
                               QStringLiteral("example.operation.issue-judgment"));
            guards.replace(2, order_guard);
            operation.insert(QStringLiteral("preconditions"), guards);
            operations.replace(index, operation);
        }
        resource.document.insert(QStringLiteral("operations"), operations);
    }));
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        auto operation = operations.at(0).toObject();
        operation.insert(QStringLiteral("expected_argument_date"), QStringLiteral("2026-10-10"));
        operations.replace(0, operation);
        resource.document.insert(QStringLiteral("operations"), operations);
    }));
    const auto set_judgment_plan_binding = [&](auto& forged, const QString& plan_id) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.issue-judgment")) {
                continue;
            }
            operation.insert(QStringLiteral("disposition_plan_id"), plan_id);
            operations.replace(index, operation);
        }
        resource.document.insert(QStringLiteral("operations"), operations);
    };
    QVERIFY(graph_rejects([&](auto& forged) {
        set_judgment_plan_binding(forged, QStringLiteral("example.disposition.missing"));
    }));
    auto unresolved_binding = *loaded;
    set_judgment_plan_binding(unresolved_binding, QStringLiteral("example.disposition.missing"));
    const auto unresolved_runtime = appellate::packs::loadRuntimePack(unresolved_binding);
    QVERIFY(!unresolved_runtime.has_value());
    QCOMPARE(unresolved_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
    const auto bind_authored_to_alternate = [&](auto& forged) {
        auto& resource = case_resource(forged);
        auto plans = resource.document.value(QStringLiteral("disposition_plans")).toArray();
        auto alternate = plans.at(0).toObject();
        alternate.insert(QStringLiteral("plan_id"),
                         QStringLiteral("example.disposition.alternate"));
        alternate.insert(
            QStringLiteral("digest"),
            QStringLiteral("30769212c7337535a2e84582a13b5385f88e9a7b38a013308fdde5499f904e1e"));
        plans.push_back(alternate);
        resource.document.insert(QStringLiteral("disposition_plans"), plans);
        set_judgment_plan_binding(forged, QStringLiteral("example.disposition.alternate"));
    };
    QVERIFY(graph_rejects(bind_authored_to_alternate));
    auto mismatched_authored_binding = *loaded;
    bind_authored_to_alternate(mismatched_authored_binding);
    const auto mismatched_authored_runtime =
        appellate::packs::loadRuntimePack(mismatched_authored_binding);
    QVERIFY(!mismatched_authored_runtime.has_value());
    QCOMPARE(mismatched_authored_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
    const auto bind_nonjudgment = [&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        auto operation = operations.at(0).toObject();
        operation.insert(QStringLiteral("disposition_plan_id"),
                         QStringLiteral("example.disposition.fictional"));
        operations.replace(0, operation);
        resource.document.insert(QStringLiteral("operations"), operations);
    };
    QVERIFY(graph_rejects(bind_nonjudgment));
    auto nonjudgment_binding = *loaded;
    bind_nonjudgment(nonjudgment_binding);
    const auto nonjudgment_runtime = appellate::packs::loadRuntimePack(nonjudgment_binding);
    QVERIFY(!nonjudgment_runtime.has_value());
    QCOMPARE(nonjudgment_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::InvalidResource);
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.issue-judgment")) {
                continue;
            }
            auto guards = operation.value(QStringLiteral("preconditions")).toArray();
            auto guard = guards.at(1).toObject();
            guard.insert(QStringLiteral("actor_id"), QStringLiteral("example.actor.appellee"));
            guards.replace(1, guard);
            operation.insert(QStringLiteral("preconditions"), guards);
            operations.replace(index, operation);
        }
        resource.document.insert(QStringLiteral("operations"), operations);
    }));
    QVERIFY(graph_rejects([&](auto& forged) {
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.issue-judgment")) {
                continue;
            }
            auto binding = operation.value(QStringLiteral("document_binding")).toObject();
            binding.insert(QStringLiteral("expected_court_date"), QStringLiteral("2026-01-04"));
            operation.insert(QStringLiteral("document_binding"), binding);
            operations.replace(index, operation);
        }
        resource.document.insert(QStringLiteral("operations"), operations);
    }));
    for (int sealed_variant = 0; sealed_variant < 3; ++sealed_variant) {
        QVERIFY(graph_rejects([&](auto& forged) {
            auto& resource = record_resource(forged);
            auto entries = resource.document.value(QStringLiteral("docket_entries")).toArray();
            auto entry = entries.at(1).toObject();
            if (sealed_variant == 0) {
                entry.insert(QStringLiteral("sealed"), true);
            } else if (sealed_variant == 1) {
                entry.remove(QStringLiteral("sealed"));
            } else {
                entry.insert(QStringLiteral("sealed"), QStringLiteral("false"));
            }
            entries.replace(1, entry);
            resource.document.insert(QStringLiteral("docket_entries"), entries);
        }));
    }

    const auto legacy =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    QVERIFY2(legacy.has_value(), legacy ? "" : qPrintable(legacy.error().message));
    for (int variant = 0; variant < 5; ++variant) {
        auto forged = *legacy;
        auto& resource = workflow_resource(forged);
        auto operations = resource.document.value(QStringLiteral("operations")).toArray();
        auto routes = resource.document.value(QStringLiteral("filing_routes")).toArray();
        if (variant == 0) {
            auto route = routes.at(0).toObject();
            route.insert(QStringLiteral("authorized_role_scope"), QStringLiteral("catalog_subset"));
            routes.replace(0, route);
            resource.document.insert(QStringLiteral("filing_routes"), routes);
        } else if (variant == 1) {
            auto route = routes.at(0).toObject();
            auto plan = route.value(QStringLiteral("deficiency_deadline")).toObject();
            plan.insert(QStringLiteral("id_mode"), QStringLiteral("exact"));
            plan.insert(
                QStringLiteral("trigger_filing"),
                QJsonObject{
                    {QStringLiteral("filing_id"), QStringLiteral("example.filing.legacy-trigger")},
                    {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
                    {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                    {QStringLiteral("document_sha256"),
                     QStringLiteral(
                         "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
                    {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-02")}});
            route.insert(QStringLiteral("deficiency_deadline"), plan);
            routes.replace(0, route);
            resource.document.insert(QStringLiteral("filing_routes"), routes);
        } else if (variant == 2) {
            auto operation = operations.at(operations.size() - 1).toObject();
            operation.insert(
                QStringLiteral("document_binding"),
                QJsonObject{
                    {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                    {QStringLiteral("document_sha256"),
                     QStringLiteral(
                         "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
                    {QStringLiteral("expected_court_date"), QStringLiteral("2026-01-02")}});
            operations.replace(operations.size() - 1, operation);
            resource.document.insert(QStringLiteral("operations"), operations);
        } else if (variant == 3) {
            auto operation = operations.at(operations.size() - 1).toObject();
            operation.insert(QStringLiteral("operation_id"),
                             QStringLiteral("example.operation.legacy-schedule"));
            operation.insert(QStringLiteral("opcode"), QStringLiteral("schedule_argument"));
            operation.remove(QStringLiteral("preconditions"));
            operation.insert(QStringLiteral("expected_argument_date"),
                             QStringLiteral("2026-10-10"));
            operations.push_back(operation);
            resource.document.insert(QStringLiteral("operations"), operations);
        } else {
            auto operation = operations.at(operations.size() - 1).toObject();
            operation.insert(QStringLiteral("disposition_plan_id"),
                             QStringLiteral("example.disposition.fictional"));
            operations.replace(operations.size() - 1, operation);
            resource.document.insert(QStringLiteral("operations"), operations);
        }
        const auto graph = appellate::packs::PackReader::validateResolvedGraph(
            forged, std::span<const appellate::packs::LoadedPack* const>{});
        QVERIFY(!graph.has_value());
        QCOMPARE(graph.error().code, appellate::packs::ErrorCode::CrossReferenceFailure);
        const auto runtime = appellate::packs::loadRuntimePack(forged);
        QVERIFY(!runtime.has_value());
        QCOMPARE(runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);
    }
}

} // namespace

QTEST_GUILESS_MAIN(PackReaderTest)

#include "tst_pack_reader.moc"
