#include "appellate/engine/oral_argument_engine.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/asset_store.hpp"
#include "appellate/storage/session_store.hpp"
#include "installed_record_controller.hpp"
#include "oral_argument_session_controller.hpp"
#include "pack_catalog_p.hpp"
#include "pack_cli.hpp"
#include "record_workspace.hpp"
#include "resolved_session_pins.hpp"
#include "session_controller.hpp"
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

struct LegacyOralDefinitions final {
    appellate::model::OralArgumentConfiguration configuration;
    appellate::model::BenchConfiguration bench;
    appellate::model::ArgumentGrounding grounding;
};

[[nodiscard]] LegacyOralDefinitions legacyOralDefinitions() {
    auto profile = appellate::model::JudgeProfile{
        "fictional.legacy-resolved",
        "Legacy Resolved Composite",
        appellate::model::ProfileClass::FictionalComposite,
        appellate::model::ProfileCompatibility{{appellate::model::CourtRole::Appellate},
                                               {"us.ca4"}},
        appellate::model::InteractionStyle{
            0.8,
            0.8,
            0.4,
            0.5,
            0.5,
            0.2,
            0.5,
            0.5,
            0.5,
            {appellate::model::IssueFocus{"issue.legacy-resolved", 1.0}}},
        appellate::model::VoiceStyle{appellate::model::VoiceRegister::Technical,
                                     appellate::model::VoiceCadence::Measured,
                                     appellate::model::QuestionFraming::Direct,
                                     appellate::model::CounselAddress::Counsel,
                                     0.4,
                                     0.4,
                                     {"answer the question"},
                                     {"before you continue"},
                                     {"clarify that point"}},
    };
    appellate::model::BenchConfiguration bench{
        "us.ca4",
        appellate::model::CourtRole::Appellate,
        {appellate::model::BenchSeat{"seat.legacy-resolved", std::move(profile)}},
        "seat.legacy-resolved"};
    appellate::model::ArgumentGrounding grounding{{appellate::model::ArgumentIssue{
        "issue.legacy-resolved",
        "legacy resolved issue",
        {{appellate::model::GroundingKind::Authority, "authority.legacy-resolved", std::nullopt}},
        {"What is the governing legacy rule?"},
        {}}}};
    const auto behavior = appellate::engine::behaviorDefinitionDigest(bench);
    const auto grounding_digest = appellate::engine::groundingDigest(grounding);
    Q_ASSERT(behavior.has_value());
    Q_ASSERT(grounding_digest.has_value());
    return LegacyOralDefinitions{appellate::model::OralArgumentConfiguration{
                                     std::chrono::seconds{90}, std::chrono::seconds{0}, 0.7, 3,
                                     *behavior, *grounding_digest, std::string(64, 'a'),
                                     "disposition.legacy-resolved"},
                                 std::move(bench), std::move(grounding)};
}

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
    void rejectsGroundedQuestionBankTargetingDependencyCase();
    void rejectsSiblingAssistedDependencyReference();
    void rejectsSiblingOnlyProcedureAuthoritySet();
    void rejectsAuthorityOutsideRootProcedureSets();
    void rejectsSiblingInvisibleDisclosureAuthority();
    void derivesSealedRecordAccessFromExactResolvedRoot();
    void rejectsWrongExactDigestForBlobStreaming();
    void serializesPublicationAcrossCatalogInstances();
    void rollsBackNewArchiveAndBlobAfterFinalizationFailure();
    void preservesCommittedInstallAfterReportedFinalizationFailure();
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

[[nodiscard]] bool overwriteAll(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
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

[[nodiscard]] auto buildPartitionArchive(
    const QString& root, const QString& stem, const QString& pack_id,
    const std::vector<QString>& resource_paths, const std::vector<PackRevision>& dependencies = {},
    bool include_blob = false, const QByteArray& replacement_prefix = {}, int schema_version = 1,
    bool omit_grounded_questions_capability = false,
    const QString& version = QStringLiteral("1.0.0")) -> std::expected<PackRevision, QString> {
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
    auto required_capabilities =
        fixture_manifest.value(QStringLiteral("required_capabilities")).toArray();
    if (omit_grounded_questions_capability) {
        QJsonArray filtered;
        for (const auto& value : required_capabilities) {
            if (value.toObject().value(QStringLiteral("id")).toString() !=
                QStringLiteral("workbench.pack.grounded-questions")) {
                filtered.push_back(value);
            }
        }
        required_capabilities = filtered;
    }
    const auto manifest =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("schema_version"), schema_version},
                          {QStringLiteral("pack_id"), pack_id},
                          {QStringLiteral("version"), version},
                          {QStringLiteral("required_capabilities"), required_capabilities},
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

[[nodiscard]] bool rewritePartitionResource(const QString& source, const QString& path,
                                            const QJsonObject& document) {
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (!overwriteAll(QDir(source).filePath(path), bytes)) {
        return false;
    }
    const auto manifest_path = QDir(source).filePath(QStringLiteral("manifest.json"));
    auto manifest = QJsonDocument::fromJson(readAll(manifest_path)).object();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto descriptor = contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("path")).toString() != path) {
            continue;
        }
        descriptor.insert(QStringLiteral("sha256"), QString::fromLatin1(sha256(bytes)));
        contents.replace(index, descriptor);
        manifest.insert(QStringLiteral("contents"), contents);
        return overwriteAll(manifest_path, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    }
    return false;
}

[[nodiscard]] auto reexportPartition(const QString& root, const QString& stem)
    -> std::expected<PackRevision, QString> {
    const auto archive =
        QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
    if (!QFile::remove(archive)) {
        return std::unexpected(QStringLiteral("cannot replace partition archive"));
    }
    const auto exported = PackArchive::exportDirectory(
        QDir(root).filePath(QStringLiteral("sources/") + stem), archive, {},
        appellate::packs::PackValidationScope::ResolvedClosure);
    if (!exported) {
        return std::unexpected(exported.error().message);
    }
    return *exported;
}

[[nodiscard]] auto addCaseAuthoritySetToPartition(const QString& root, const QString& stem,
                                                  bool declare_for_procedure = true)
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto fixture_authority_set =
        QJsonDocument::fromJson(
            readAll(QDir(QStringLiteral(APPELLATE_TEST_FIXTURES) +
                         QStringLiteral("/full-resource-pack-v2"))
                        .filePath(QStringLiteral("resources/authority-set.json"))))
            .object();
    auto authorities = fixture_authority_set.value(QStringLiteral("authorities")).toArray();
    if (fixture_authority_set.isEmpty() || authorities.isEmpty()) {
        return std::unexpected(QStringLiteral("case authority fixture is missing"));
    }
    auto authority = authorities.first().toObject();
    authority.insert(QStringLiteral("authority_id"),
                     QStringLiteral("example.authority.case-specific"));
    authority.insert(QStringLiteral("citation"), QStringLiteral("Fictional Case Rule 7"));
    authority.insert(QStringLiteral("locator"), QStringLiteral("Rule 7"));
    authority.insert(QStringLiteral("source_url"),
                     QStringLiteral("https://example.invalid/rules/7"));
    authority.insert(QStringLiteral("proposition"),
                     QStringLiteral("The case-specific issue is reviewable."));
    auto case_authority_set = fixture_authority_set;
    case_authority_set.insert(QStringLiteral("resource_id"),
                              QStringLiteral("example.authorities.case-specific"));
    case_authority_set.insert(QStringLiteral("authorities"), QJsonArray{authority});
    const auto authority_set_path = QStringLiteral("resources/authority-set-case.json");
    const auto authority_set_bytes =
        QJsonDocument(case_authority_set).toJson(QJsonDocument::Compact);
    if (!writeAll(QDir(source).filePath(authority_set_path), authority_set_bytes)) {
        return std::unexpected(QStringLiteral("cannot add case authority set"));
    }

    const auto manifest_path = QDir(source).filePath(QStringLiteral("manifest.json"));
    auto manifest = QJsonDocument::fromJson(readAll(manifest_path)).object();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    contents.push_back(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("example.authorities.case-specific")},
        {QStringLiteral("kind"), QStringLiteral("authority_set")},
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("path"), authority_set_path},
        {QStringLiteral("sha256"), QString::fromLatin1(sha256(authority_set_bytes))},
    });
    manifest.insert(QStringLiteral("contents"), contents);
    if (!overwriteAll(manifest_path, QJsonDocument(manifest).toJson(QJsonDocument::Compact))) {
        return std::unexpected(QStringLiteral("cannot update case authority manifest"));
    }

    const auto procedure_path = QStringLiteral("resources/procedure-profile.json");
    auto procedure =
        QJsonDocument::fromJson(readAll(QDir(source).filePath(procedure_path))).object();
    auto set_ids = procedure.value(QStringLiteral("authority_set_ids")).toArray();
    if (declare_for_procedure) {
        set_ids.push_back(QStringLiteral("example.authorities.case-specific"));
        procedure.insert(QStringLiteral("authority_set_ids"), set_ids);
    }
    const auto case_path = QStringLiteral("resources/case.json");
    auto case_document =
        QJsonDocument::fromJson(readAll(QDir(source).filePath(case_path))).object();
    auto issues = case_document.value(QStringLiteral("issues")).toArray();
    if (procedure.isEmpty() || issues.isEmpty()) {
        return std::unexpected(QStringLiteral("case authority consumer fixture is missing"));
    }
    auto issue = issues.first().toObject();
    auto authority_ids = issue.value(QStringLiteral("authority_ids")).toArray();
    authority_ids.push_back(QStringLiteral("example.authority.case-specific"));
    issue.insert(QStringLiteral("authority_ids"), authority_ids);
    issues.replace(0, issue);
    case_document.insert(QStringLiteral("issues"), issues);
    if ((declare_for_procedure && !rewritePartitionResource(source, procedure_path, procedure)) ||
        !rewritePartitionResource(source, case_path, case_document)) {
        return std::unexpected(QStringLiteral("cannot bind case authority set"));
    }
    return reexportPartition(root, stem);
}

[[nodiscard]] auto addProcedureAuthoritySetReference(const QString& root, const QString& stem,
                                                     const QString& authority_set_id)
    -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto path = QStringLiteral("resources/procedure-profile.json");
    auto procedure = QJsonDocument::fromJson(readAll(QDir(source).filePath(path))).object();
    auto set_ids = procedure.value(QStringLiteral("authority_set_ids")).toArray();
    if (procedure.isEmpty() || set_ids.isEmpty()) {
        return std::unexpected(QStringLiteral("procedure authority fixture is missing"));
    }
    set_ids.push_back(authority_set_id);
    procedure.insert(QStringLiteral("authority_set_ids"), set_ids);
    if (!rewritePartitionResource(source, path, procedure)) {
        return std::unexpected(QStringLiteral("cannot rewrite procedure authority sets"));
    }
    return reexportPartition(root, stem);
}

[[nodiscard]] auto addSealedTwinsToPartition(
    const QString& root, const QString& stem, bool mutate_authority_metadata = false,
    const QString& authorization_authority_id = QStringLiteral("example.authority.deficiency"),
    bool require_missing_support = false) -> std::expected<PackRevision, QString> {
    const auto source = QDir(root).filePath(QStringLiteral("sources/") + stem);
    const auto record_path = QStringLiteral("resources/record.json");
    auto record = QJsonDocument::fromJson(readAll(QDir(source).filePath(record_path))).object();
    auto entries = record.value(QStringLiteral("docket_entries")).toArray();
    if (record.isEmpty() || entries.isEmpty()) {
        return std::unexpected(QStringLiteral("sealed-twins fixture record is missing"));
    }
    auto sealed = entries.at(0).toObject();
    sealed.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed"));
    sealed.insert(QStringLiteral("entry_number"), 3);
    sealed.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-S"));
    sealed.insert(QStringLiteral("title"), QStringLiteral("Confidential PSR title"));
    sealed.insert(QStringLiteral("description"), QStringLiteral("Secret PSR description"));
    sealed.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("psr-secret-tag")});
    sealed.insert(QStringLiteral("sealed"), true);
    entries.push_back(sealed);
    record.insert(QStringLiteral("docket_entries"), entries);
    auto anchors = record.value(QStringLiteral("page_anchors")).toArray();
    anchors.push_back(QJsonObject{
        {QStringLiteral("anchor_id"), QStringLiteral("example.record.anchor.psr-sealed")},
        {QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed")},
        {QStringLiteral("page_number"), 2},
        {QStringLiteral("citation_label"), QStringLiteral("SECRET-JA-2")},
    });
    record.insert(QStringLiteral("page_anchors"), anchors);
    QJsonArray required_items{QStringLiteral("redacted_counterpart")};
    if (require_missing_support) {
        required_items.push_back(QStringLiteral("motion"));
        required_items.push_back(QStringLiteral("certificate"));
    }
    record.insert(
        QStringLiteral("disclosure_policy"),
        QJsonObject{
            {QStringLiteral("policy_id"), QStringLiteral("example.record.policy.psr")},
            {QStringLiteral("unauthorized_projection"), QStringLiteral("public_counterparts_only")},
            {QStringLiteral("authorized_projection"),
             QStringLiteral("public_and_authorized_sealed")},
            {QStringLiteral("sealed_asset_access"), QStringLiteral("session_event_grant_required")},
        });
    record.insert(
        QStringLiteral("sealed_disclosures"),
        QJsonArray{QJsonObject{
            {QStringLiteral("disclosure_id"), QStringLiteral("example.disclosure.psr")},
            {QStringLiteral("sealed_entry_id"), QStringLiteral("example.record.psr-sealed")},
            {QStringLiteral("public_entry_id"), QStringLiteral("example.record.entry-one")},
            {QStringLiteral("authorization_authority_id"), authorization_authority_id},
            {QStringLiteral("required_items"), required_items},
            {QStringLiteral("anchor_mappings"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("stable_anchor_id"),
                  QStringLiteral("example.record.anchor.psr-stable")},
                 {QStringLiteral("sealed_anchor_id"),
                  QStringLiteral("example.record.anchor.psr-sealed")},
                 {QStringLiteral("public_anchor_id"), QStringLiteral("example.record.anchor.ja2")},
             }}},
        }});
    if (!rewritePartitionResource(source, record_path, record)) {
        return std::unexpected(QStringLiteral("cannot rewrite sealed-twins record"));
    }

    if (mutate_authority_metadata) {
        const auto authority_path = QStringLiteral("resources/authority-set.json");
        auto authority_set =
            QJsonDocument::fromJson(readAll(QDir(source).filePath(authority_path))).object();
        auto authorities = authority_set.value(QStringLiteral("authorities")).toArray();
        for (qsizetype index = 0; index < authorities.size(); ++index) {
            auto authority = authorities.at(index).toObject();
            if (authority.value(QStringLiteral("authority_id")).toString() !=
                QStringLiteral("example.authority.deficiency")) {
                continue;
            }
            authority.insert(QStringLiteral("source_url"),
                             QStringLiteral("https://example.invalid/rules/3-revised"));
            authorities.replace(index, authority);
            break;
        }
        authority_set.insert(QStringLiteral("authorities"), authorities);
        if (!rewritePartitionResource(source, authority_path, authority_set)) {
            return std::unexpected(QStringLiteral("cannot mutate disclosure authority"));
        }
    }

    const auto manifest_path = QDir(source).filePath(QStringLiteral("manifest.json"));
    auto manifest = QJsonDocument::fromJson(readAll(manifest_path)).object();
    auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
    capabilities.push_back(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.sealed-record-twins")},
        {QStringLiteral("version"), 1},
    });
    manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    if (!overwriteAll(manifest_path, QJsonDocument(manifest).toJson(QJsonDocument::Compact))) {
        return std::unexpected(QStringLiteral("cannot add sealed-twins capability"));
    }
    const auto archive =
        QDir(root).filePath(QStringLiteral("archives/") + stem + QStringLiteral(".awpack"));
    if (!QFile::remove(archive)) {
        return std::unexpected(QStringLiteral("cannot replace base sealed-twins archive"));
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

    catalog->reset();

    // This simulates catalog loss from an older/crashed installation. Direct rows in the
    // verified middle archive still pin the leaf. B0b reverse admission rejects the incoherent
    // database before exposing any new handle.
    QVERIFY(executeCatalogSql(
        catalog_root,
        QStringLiteral("DELETE FROM pack_revisions WHERE pack_id = 'test.missing.leaf'")));
    const auto reopened = PackCatalog::open(catalog_root);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, CatalogErrorCode::CorruptCatalog);
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

    catalog->reset();

    QVERIFY(executeCatalogSql(
        catalog_root, QStringLiteral("UPDATE pack_dependencies SET dependency_digest = '%1' "
                                     "WHERE pack_id = 'test.dep.root'")
                          .arg(QString(64, u'a'))));
    const auto reopened = PackCatalog::open(catalog_root);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error().code, CatalogErrorCode::CorruptCatalog);
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
    const auto every_file = QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    const auto catalog_archives = QDir(catalog_root).filePath(QStringLiteral("archives"));
    const auto catalog_blobs = QDir(catalog_root).filePath(QStringLiteral("blobs"));
    const auto archive_names = QDir(catalog_archives).entryList({}, every_file, QDir::Name);
    QCOMPARE(archive_names.size(), 129);
    for (const auto& name : archive_names) {
        QVERIFY(name.endsWith(QStringLiteral(".awpack")));
        QCOMPARE(name.size(), 64 + qsizetype{7});
    }
    QVERIFY(QDir(catalog_blobs).entryList({}, every_file).empty());
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock"))));
    QVERIFY(
        !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"))));
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
         QStringLiteral("resources/record.json")},
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
    const auto oral_database =
        QDir(session_directory.path()).filePath(QStringLiteral("legacy-oral.sqlite"));
    {
        auto definitions = legacyOralDefinitions();
        auto store = appellate::storage::SessionStore::open(oral_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto created = appellate::app::OralArgumentSessionController::create(
            QStringLiteral("test.session.legacy-resolved"), std::move(definitions.configuration),
            std::move(definitions.bench), std::move(definitions.grounding), std::move(*store),
            QStringLiteral("engine.legacy-resolved.1"), QStringLiteral("2026-08-11T11:00:00Z"),
            *resolved);
        QVERIFY2(created.has_value(), created ? "" : qPrintable(created.error().message));
        QVERIFY((*created)->snapshot().pins == pins);
        QCOMPARE((*created)->snapshot().authority_contract,
                 appellate::storage::SessionAuthorityContract::LegacyV1);
    }
    {
        auto definitions = legacyOralDefinitions();
        auto store = appellate::storage::SessionStore::open(oral_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto reopened = appellate::app::OralArgumentSessionController::reopen(
            QStringLiteral("test.session.legacy-resolved"), std::move(definitions.configuration),
            std::move(definitions.bench), std::move(definitions.grounding), std::move(*store),
            QStringLiteral("engine.legacy-resolved.1"), *resolved);
        QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error().message));
        QVERIFY((*reopened)->snapshot().pins == pins);
    }
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
        {QStringLiteral("resources/authority-set.json")}, {}, false, {}, 2, false,
        QStringLiteral("2024.02.29"));
    QVERIFY2(authorities.has_value(), authorities ? "" : qPrintable(authorities.error()));
    const auto procedure = buildPartitionArchive(
        temporary.path(), QStringLiteral("v2-procedure"), QStringLiteral("test.v2.procedure"),
        {QStringLiteral("resources/court.json"), QStringLiteral("resources/filing-catalog.json"),
         QStringLiteral("resources/form.json"), QStringLiteral("resources/workflow.json")},
        {*authorities}, false, {}, 2, false, QStringLiteral("2026.03.22"));
    QVERIFY2(procedure.has_value(), procedure ? "" : qPrintable(procedure.error()));
    const auto root_base = buildPartitionArchive(
        temporary.path(), QStringLiteral("v2-root"), QStringLiteral("test.v2.root"),
        {QStringLiteral("resources/argument-config-counterfactual.json"),
         QStringLiteral("resources/argument-config.json"),
         QStringLiteral("resources/bench-configuration.json"),
         QStringLiteral("resources/case.json"), QStringLiteral("resources/judge-profile.json"),
         QStringLiteral("resources/procedure-profile.json"),
         QStringLiteral("resources/record.json")},
        {*procedure}, true, {}, 2, false, QStringLiteral("2026.03.23"));
    QVERIFY2(root_base.has_value(), root_base ? "" : qPrintable(root_base.error()));
    const auto root = addCaseAuthoritySetToPartition(temporary.path(), QStringLiteral("v2-root"));
    QVERIFY2(root.has_value(), root ? "" : qPrintable(root.error()));
    QCOMPARE(authorities->version, std::string("2024.02.29"));
    QCOMPARE(procedure->version, std::string("2026.03.22"));
    QCOMPARE(root->version, std::string("2026.03.23"));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-authorities"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-procedure"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("v2-root"), 3));
    const auto resolved = (*catalog)->loadResolved(*root);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto resolved_pins = appellate::app::revisionPinsForSession(*resolved);
    QCOMPARE(resolved_pins.size(), std::size_t{3});
    const auto authority_pin =
        std::ranges::find(resolved_pins, QStringLiteral("test.v2.authorities"),
                          &appellate::storage::RevisionPin::pack_id);
    const auto procedure_pin = std::ranges::find(resolved_pins, QStringLiteral("test.v2.procedure"),
                                                 &appellate::storage::RevisionPin::pack_id);
    const auto root_pin = std::ranges::find(resolved_pins, QStringLiteral("test.v2.root"),
                                            &appellate::storage::RevisionPin::pack_id);
    QVERIFY(authority_pin != resolved_pins.end());
    QVERIFY(procedure_pin != resolved_pins.end());
    QVERIFY(root_pin != resolved_pins.end());
    QCOMPARE(authority_pin->version, QStringLiteral("2024.02.29"));
    QCOMPARE(procedure_pin->version, QStringLiteral("2026.03.22"));
    QCOMPARE(root_pin->version, QStringLiteral("2026.03.23"));
    QCOMPARE(resolved->resourceOwner("example.authorities.fictional"),
             std::optional<PackRevision>(*authorities));
    QCOMPARE(resolved->resourceOwner("example.procedure.fictional"),
             std::optional<PackRevision>(*root));
    QCOMPARE(resolved->resourceOwner("example.authorities.case-specific"),
             std::optional<PackRevision>(*root));

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
    QCOMPARE(runtime_case.issues.front().authorities.size(), std::size_t{2});
    QVERIFY(runtime_case.issues.front().authorities.front().provenance.has_value());
    QCOMPARE(runtime_case.issues.front().authorities.front().citation,
             std::string("Fictional Rule 1"));
    const auto case_authority =
        std::ranges::find(runtime_case.issues.front().authorities,
                          appellate::model::AuthorityId{"example.authority.case-specific"},
                          &appellate::model::AuthorityRef::id);
    QVERIFY(case_authority != runtime_case.issues.front().authorities.end());
    QVERIFY(case_authority->provenance.has_value());
    QCOMPARE(case_authority->provenance->source_url,
             std::string("https://example.invalid/rules/7"));
    QCOMPARE(runtime_case.filing_authorities.size(), std::size_t{1});
    QCOMPARE(runtime_case.filing_authorities.front().filing_type_id.value,
             std::string("example.filing.notice"));
    QVERIFY(runtime_case.filing_authorities.front().authority.provenance.has_value());
    QCOMPARE(runtime_case.filing_authorities.front().authority.provenance->locator,
             std::string("Rule 1"));
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{2});
    const auto actual_argument =
        std::ranges::find(runtime_case.argument_configurations,
                          appellate::packs::RuntimeArgumentConfigId{"example.argument.fictional"},
                          &appellate::packs::RuntimeArgumentConfiguration::id);
    QVERIFY(actual_argument != runtime_case.argument_configurations.end());
    QVERIFY(actual_argument->grounded_question_bank.has_value());
    const auto& question_bank = *actual_argument->grounded_question_bank;
    QCOMPARE(question_bank.mode, appellate::model::OralArgumentMode::ActualRecord);
    QCOMPARE(question_bank.grounding_digest,
             std::string("766b0a05b8d4c6ed2b05496f520bc34d11ade1d1d670f7dd6fb036c11a238c55"));
    const auto grounded_authority =
        std::ranges::find_if(question_bank.questions.front().grounding, [](const auto& grounding) {
            return std::holds_alternative<appellate::model::AuthorityArgumentGrounding>(grounding);
        });
    QVERIFY(grounded_authority != question_bank.questions.front().grounding.end());
    const auto& authority_snapshot =
        std::get<appellate::model::AuthorityArgumentGrounding>(*grounded_authority).authority;
    QVERIFY(authority_snapshot.provenance.has_value());
    QCOMPARE(authority_snapshot.provenance->locator, std::string("Rule 1"));

    const auto oral_database =
        QDir(temporary.path()).filePath(QStringLiteral("v2-oral-sessions.sqlite"));
    const std::array canonical_arguments{
        std::pair{appellate::packs::RuntimeArgumentConfigId{"example.argument.fictional"},
                  appellate::model::OralArgumentMode::ActualRecord},
        std::pair{appellate::packs::RuntimeArgumentConfigId{"example.argument.counterfactual"},
                  appellate::model::OralArgumentMode::CounterfactualTraining},
    };
    for (std::size_t index = 0; index < canonical_arguments.size(); ++index) {
        const auto oral_session_id = QStringLiteral("test.session.resolved-oral-%1").arg(index);
        auto store = appellate::storage::SessionStore::open(oral_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        auto created = appellate::app::OralArgumentSessionController::create(
            oral_session_id, runtime_case.definition.id, canonical_arguments[index].first,
            std::string(64, 'c'), std::move(*store), QStringLiteral("engine.resolved-oral.2"),
            QStringLiteral("2026-08-11T10:00:00Z"), *resolved);
        QVERIFY2(created.has_value(), created ? "" : qPrintable(created.error().message));
        QVERIFY((*created)->canonicalDefinition() != nullptr);
        QCOMPARE((*created)->canonicalDefinition()->question_bank.mode,
                 canonical_arguments[index].second);
        QCOMPARE((*created)->snapshot().authority_contract,
                 appellate::storage::SessionAuthorityContract::CanonicalV2);
        QVERIFY((*created)->snapshot().pins == resolved_pins);
        created->reset();

        store = appellate::storage::SessionStore::open(oral_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto reopened = appellate::app::OralArgumentSessionController::reopen(
            oral_session_id, runtime_case.definition.id, canonical_arguments[index].first,
            std::string(64, 'c'), std::move(*store), QStringLiteral("engine.resolved-oral.2"),
            *resolved);
        QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error().message));
        QCOMPARE((*reopened)->snapshot().authority_contract,
                 appellate::storage::SessionAuthorityContract::CanonicalV2);
        QVERIFY((*reopened)->snapshot().pins == resolved_pins);
    }
    {
        auto definitions = legacyOralDefinitions();
        auto store = appellate::storage::SessionStore::open(oral_database);
        QVERIFY2(store.has_value(), store ? "" : qPrintable(store.error().message));
        const auto rejected = appellate::app::OralArgumentSessionController::create(
            QStringLiteral("test.session.v2-legacy-refused"), std::move(definitions.configuration),
            std::move(definitions.bench), std::move(definitions.grounding), std::move(*store),
            QStringLiteral("engine.legacy-resolved.1"), QStringLiteral("2026-08-11T10:00:00Z"),
            *resolved);
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code,
                 appellate::app::OralArgumentSessionErrorCode::InvalidConfiguration);
    }

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
        QVERIFY((*created)->snapshot().pins == resolved_pins);
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
        QCOMPARE(downgraded.error().code,
                 appellate::app::WorkflowSessionErrorCode::InvalidConfiguration);
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

void PackDependencyResolutionTest::rejectsGroundedQuestionBankTargetingDependencyCase() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto authorities =
        buildPartitionArchive(temporary.path(), QStringLiteral("grounded-authorities"),
                              QStringLiteral("test.grounded.authorities"),
                              {QStringLiteral("resources/authority-set.json")}, {}, false, {}, 2);
    QVERIFY2(authorities.has_value(), authorities ? "" : qPrintable(authorities.error()));
    const auto procedure = buildPartitionArchive(
        temporary.path(), QStringLiteral("grounded-procedure"),
        QStringLiteral("test.grounded.procedure"),
        {QStringLiteral("resources/court.json"), QStringLiteral("resources/filing-catalog.json"),
         QStringLiteral("resources/form.json"), QStringLiteral("resources/procedure-profile.json"),
         QStringLiteral("resources/workflow.json")},
        {*authorities}, false, {}, 2);
    QVERIFY2(procedure.has_value(), procedure ? "" : qPrintable(procedure.error()));
    const auto case_provider = buildPartitionArchive(
        temporary.path(), QStringLiteral("grounded-case-provider"),
        QStringLiteral("test.grounded.case-provider"),
        {QStringLiteral("resources/bench-configuration.json"),
         QStringLiteral("resources/case.json"), QStringLiteral("resources/judge-profile.json"),
         QStringLiteral("resources/record.json")},
        {*procedure}, true, {}, 2);
    QVERIFY2(case_provider.has_value(), case_provider ? "" : qPrintable(case_provider.error()));
    const auto root = buildPartitionArchive(
        temporary.path(), QStringLiteral("grounded-root"), QStringLiteral("test.grounded.root"),
        {QStringLiteral("resources/argument-config.json")}, {*case_provider}, false, {}, 2);
    QVERIFY2(root.has_value(), root ? "" : qPrintable(root.error()));

    const auto underdeclared = buildPartitionArchive(
        temporary.path(), QStringLiteral("grounded-underdeclared"),
        QStringLiteral("test.grounded.underdeclared"),
        {QStringLiteral("resources/argument-config.json")}, {*case_provider}, false, {}, 2, true);
    QVERIFY(!underdeclared.has_value());
    QVERIFY(underdeclared.error().contains(QStringLiteral("grounded-questions")));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("grounded-authorities"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("grounded-procedure"), 2));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("grounded-case-provider"), 3));
    const auto rejected =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("grounded-root")),
                                   QStringLiteral("2026-08-11T00:00:04Z"));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::InvalidResolvedGraph);
    const auto installed = (*catalog)->list();
    QVERIFY(installed.has_value());
    QCOMPARE(installed->size(), std::size_t{3});
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

void PackDependencyResolutionTest::rejectsSiblingOnlyProcedureAuthoritySet() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto support = buildPartitionArchive(
        temporary.path(), QStringLiteral("authority-visibility-support"),
        QStringLiteral("test.authority-visibility.support"),
        {QStringLiteral("resources/authority-set.json"), QStringLiteral("resources/court.json"),
         QStringLiteral("resources/filing-catalog.json"), QStringLiteral("resources/form.json"),
         QStringLiteral("resources/workflow.json")},
        {}, false, {}, 2);
    QVERIFY2(support.has_value(), support ? "" : qPrintable(support.error()));
    const auto sibling = buildPartitionArchive(
        temporary.path(), QStringLiteral("authority-visibility-sibling"),
        QStringLiteral("test.authority-visibility.sibling"),
        {QStringLiteral("resources/authority-set.json")}, {}, false, QByteArray("sibling."), 2);
    QVERIFY2(sibling.has_value(), sibling ? "" : qPrintable(sibling.error()));
    const auto dependent_base = buildPartitionArchive(
        temporary.path(), QStringLiteral("authority-visibility-dependent"),
        QStringLiteral("test.authority-visibility.dependent"),
        {QStringLiteral("resources/procedure-profile.json")}, {*support}, false, {}, 2);
    QVERIFY2(dependent_base.has_value(), dependent_base ? "" : qPrintable(dependent_base.error()));
    const auto dependent = addProcedureAuthoritySetReference(
        temporary.path(), QStringLiteral("authority-visibility-dependent"),
        QStringLiteral("sibling.authorities.fictional"));
    QVERIFY2(dependent.has_value(), dependent ? "" : qPrintable(dependent.error()));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(
        install(**catalog, temporary.path(), QStringLiteral("authority-visibility-support"), 1));
    QVERIFY(
        install(**catalog, temporary.path(), QStringLiteral("authority-visibility-sibling"), 2));
    const auto rejected = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("authority-visibility-dependent")),
        QStringLiteral("2026-08-11T00:00:03Z"));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::InvalidResolvedGraph);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{2});
}

void PackDependencyResolutionTest::rejectsAuthorityOutsideRootProcedureSets() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto support = buildPartitionArchive(
        temporary.path(), QStringLiteral("authority-scope-support"),
        QStringLiteral("test.authority-scope.support"),
        {QStringLiteral("resources/authority-set.json"), QStringLiteral("resources/court.json"),
         QStringLiteral("resources/filing-catalog.json"), QStringLiteral("resources/form.json"),
         QStringLiteral("resources/workflow.json")},
        {}, false, {}, 2);
    QVERIFY2(support.has_value(), support ? "" : qPrintable(support.error()));
    const auto root_base = buildPartitionArchive(
        temporary.path(), QStringLiteral("authority-scope-root"),
        QStringLiteral("test.authority-scope.root"),
        {QStringLiteral("resources/bench-configuration.json"),
         QStringLiteral("resources/case.json"), QStringLiteral("resources/judge-profile.json"),
         QStringLiteral("resources/procedure-profile.json"),
         QStringLiteral("resources/record.json")},
        {*support}, true, {}, 2);
    QVERIFY2(root_base.has_value(), root_base ? "" : qPrintable(root_base.error()));
    const auto root = addCaseAuthoritySetToPartition(temporary.path(),
                                                     QStringLiteral("authority-scope-root"), false);
    QVERIFY2(root.has_value(), root ? "" : qPrintable(root.error()));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("authority-scope-support"), 1));
    const auto rejected = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("authority-scope-root")),
        QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error().code, CatalogErrorCode::InvalidResolvedGraph);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{1});
}

void PackDependencyResolutionTest::rejectsSiblingInvisibleDisclosureAuthority() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto support = buildPartitionArchive(
        temporary.path(), QStringLiteral("sealed-record-support"),
        QStringLiteral("test.sealed.record-support"),
        {QStringLiteral("resources/authority-set.json"), QStringLiteral("resources/court.json")},
        {}, false, {}, 2);
    QVERIFY2(support.has_value(), support ? "" : qPrintable(support.error()));
    const auto sibling = buildPartitionArchive(
        temporary.path(), QStringLiteral("sealed-authority-sibling"),
        QStringLiteral("test.sealed.authority-sibling"),
        {QStringLiteral("resources/authority-set.json")}, {}, false, QByteArray("sibling."), 2);
    QVERIFY2(sibling.has_value(), sibling ? "" : qPrintable(sibling.error()));
    const auto dependent =
        buildPartitionArchive(temporary.path(), QStringLiteral("sealed-record-dependent"),
                              QStringLiteral("test.sealed.record-dependent"),
                              {QStringLiteral("resources/record.json")}, {*support}, true, {}, 2);
    QVERIFY2(dependent.has_value(), dependent ? "" : qPrintable(dependent.error()));
    const auto sealed_dependent =
        addSealedTwinsToPartition(temporary.path(), QStringLiteral("sealed-record-dependent"),
                                  false, QStringLiteral("sibling.authority.deficiency"));
    QVERIFY2(sealed_dependent.has_value(),
             sealed_dependent ? "" : qPrintable(sealed_dependent.error()));

    auto catalog = PackCatalog::open(QDir(temporary.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("sealed-record-support"), 1));
    QVERIFY(install(**catalog, temporary.path(), QStringLiteral("sealed-authority-sibling"), 2));
    const auto installed = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("sealed-record-dependent")),
        QStringLiteral("2026-08-11T00:00:03Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::InvalidResolvedGraph);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QCOMPARE(listed->size(), std::size_t{2});
}

void PackDependencyResolutionTest::derivesSealedRecordAccessFromExactResolvedRoot() {
    const std::vector<QString> resources{
        QStringLiteral("resources/argument-config.json"),
        QStringLiteral("resources/argument-config-counterfactual.json"),
        QStringLiteral("resources/authority-set.json"),
        QStringLiteral("resources/bench-configuration.json"),
        QStringLiteral("resources/case.json"),
        QStringLiteral("resources/court.json"),
        QStringLiteral("resources/filing-catalog.json"),
        QStringLiteral("resources/form.json"),
        QStringLiteral("resources/judge-profile.json"),
        QStringLiteral("resources/procedure-profile.json"),
        QStringLiteral("resources/record.json"),
        QStringLiteral("resources/workflow.json"),
    };

    QTemporaryDir exact;
    QVERIFY(exact.isValid());
    const auto base =
        buildPartitionArchive(exact.path(), QStringLiteral("sealed-root"),
                              QStringLiteral("test.sealed.root"), resources, {}, true, {}, 2);
    QVERIFY2(base.has_value(), base ? "" : qPrintable(base.error()));
    const auto revision = addSealedTwinsToPartition(exact.path(), QStringLiteral("sealed-root"));
    QVERIFY2(revision.has_value(), revision ? "" : qPrintable(revision.error()));
    auto catalog = PackCatalog::open(QDir(exact.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(catalog.has_value());
    QVERIFY(install(**catalog, exact.path(), QStringLiteral("sealed-root"), 1));
    const auto resolved = (*catalog)->loadResolved(*revision);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*resolved);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());

    const auto database_path = QDir(exact.path()).filePath(QStringLiteral("record-access.sqlite"));
    auto store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    auto controller = appellate::app::RecordAccessSessionController::create(
        QStringLiteral("test.session.sealed-root"), runtime->cases.front().definition.id,
        std::move(*store), QStringLiteral("engine.record-access.v1"),
        QStringLiteral("2026-08-11T10:00:00Z"), *resolved);
    QVERIFY2(controller.has_value(), controller ? "" : qPrintable(controller.error().message));
    const auto disclosures = (*controller)->disclosures();
    QCOMPARE(disclosures.size(), std::size_t{1});
    QCOMPARE(disclosures.front().disclosure_id, std::string("example.disclosure.psr"));
    QVERIFY(disclosures.front().blocking_deficiencies.empty());
    QVERIFY(!disclosures.front().authorized);
    const auto raw_target = (*controller)
                                ->grant("example.record.psr-sealed", "test.event.raw-target",
                                        QStringLiteral("2026-08-11T10:00:30Z"));
    QVERIFY(!raw_target.has_value());
    QCOMPARE(raw_target.error().code,
             appellate::app::SessionControllerErrorCode::InvalidConfiguration);
    const auto granted = (*controller)
                             ->grant("example.disclosure.psr", "test.event.disclosure-grant",
                                     QStringLiteral("2026-08-11T10:01:00Z"));
    QVERIFY2(granted.has_value(), granted ? "" : qPrintable(granted.error().message));
    QVERIFY((*controller)->disclosures().front().authorized);
    controller->reset();

    auto owner_store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(owner_store.has_value());
    auto store_a = (*owner_store)->forkConnection();
    QVERIFY(store_a.has_value());
    auto controller_a = appellate::app::RecordAccessSessionController::reopen(
        QStringLiteral("test.session.sealed-root"), runtime->cases.front().definition.id,
        std::move(*store_a), QStringLiteral("engine.record-access.v1"), *resolved);
    QVERIFY2(controller_a.has_value(),
             controller_a ? "" : qPrintable(controller_a.error().message));
    QVERIFY((*controller_a)->disclosures().front().authorized);

    auto store_b = (*owner_store)->forkConnection();
    QVERIFY(store_b.has_value());
    auto controller_b = appellate::app::RecordAccessSessionController::reopen(
        QStringLiteral("test.session.sealed-root"), runtime->cases.front().definition.id,
        std::move(*store_b), QStringLiteral("engine.record-access.v1"), *resolved);
    QVERIFY2(controller_b.has_value(),
             controller_b ? "" : qPrintable(controller_b.error().message));
    const auto revoked = (*controller_b)
                             ->revoke("example.disclosure.psr", "test.event.disclosure-revoke",
                                      QStringLiteral("2026-08-11T10:01:30Z"));
    QVERIFY2(revoked.has_value(), revoked ? "" : qPrintable(revoked.error().message));

    appellate::ui::RecordWorkspace fresh_workspace;
    appellate::app::InstalledRecordController installed_controller(**catalog, fresh_workspace);
    const auto installed =
        installed_controller.load(*resolved, *runtime, runtime->cases.front().definition.id);
    QVERIFY2(installed.has_value(), installed ? "" : qPrintable(installed.error().message));
    const auto applied = (*controller_a)->applyCurrentProjection(fresh_workspace);
    QVERIFY2(applied.has_value(), applied ? "" : qPrintable(applied.error().message));
    QVERIFY(!(*controller_a)->disclosures().front().authorized);
    QVERIFY(
        !fresh_workspace.openDocketEntry(QStringLiteral("example.record.psr-sealed")).has_value());
    QVERIFY(fresh_workspace.navigateToAnchor(QStringLiteral("example.record.anchor.psr-stable"))
                .has_value());
    QCOMPARE(fresh_workspace.currentDocumentId(), QStringLiteral("example.record.entry-one"));
    controller_a->reset();
    controller_b->reset();
    owner_store->reset();

    QTemporaryDir revised;
    QVERIFY(revised.isValid());
    const auto revised_base =
        buildPartitionArchive(revised.path(), QStringLiteral("sealed-root"),
                              QStringLiteral("test.sealed.root"), resources, {}, true, {}, 2);
    QVERIFY2(revised_base.has_value(), revised_base ? "" : qPrintable(revised_base.error()));
    const auto revised_revision =
        addSealedTwinsToPartition(revised.path(), QStringLiteral("sealed-root"), true);
    QVERIFY2(revised_revision.has_value(),
             revised_revision ? "" : qPrintable(revised_revision.error()));
    QVERIFY(revised_revision->digest != revision->digest);
    auto revised_catalog =
        PackCatalog::open(QDir(revised.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(revised_catalog.has_value());
    QVERIFY(install(**revised_catalog, revised.path(), QStringLiteral("sealed-root"), 2));
    const auto revised_resolved = (*revised_catalog)->loadResolved(*revised_revision);
    QVERIFY(revised_resolved.has_value());

    store = appellate::storage::SessionStore::open(database_path);
    QVERIFY(store.has_value());
    const auto metadata_mutation = appellate::app::RecordAccessSessionController::reopen(
        QStringLiteral("test.session.sealed-root"), runtime->cases.front().definition.id,
        std::move(*store), QStringLiteral("engine.record-access.v1"), *revised_resolved);
    QVERIFY(!metadata_mutation.has_value());
    QCOMPARE(metadata_mutation.error().code,
             appellate::app::SessionControllerErrorCode::CorruptSession);

    QTemporaryDir deficient;
    QVERIFY(deficient.isValid());
    const auto deficient_base =
        buildPartitionArchive(deficient.path(), QStringLiteral("sealed-deficient"),
                              QStringLiteral("test.sealed.deficient"), resources, {}, true, {}, 2);
    QVERIFY(deficient_base.has_value());
    const auto deficient_revision =
        addSealedTwinsToPartition(deficient.path(), QStringLiteral("sealed-deficient"), false,
                                  QStringLiteral("example.authority.deficiency"), true);
    QVERIFY2(deficient_revision.has_value(),
             deficient_revision ? "" : qPrintable(deficient_revision.error()));
    auto deficient_catalog =
        PackCatalog::open(QDir(deficient.path()).filePath(QStringLiteral("catalog")));
    QVERIFY(deficient_catalog.has_value());
    QVERIFY(install(**deficient_catalog, deficient.path(), QStringLiteral("sealed-deficient"), 3));
    const auto deficient_resolved = (*deficient_catalog)->loadResolved(*deficient_revision);
    QVERIFY(deficient_resolved.has_value());
    const auto deficient_runtime = appellate::packs::loadRuntimePack(*deficient_resolved);
    QVERIFY(deficient_runtime.has_value());
    store = appellate::storage::SessionStore::open(
        QDir(deficient.path()).filePath(QStringLiteral("record-access.sqlite")));
    QVERIFY(store.has_value());
    auto deficient_controller = appellate::app::RecordAccessSessionController::create(
        QStringLiteral("test.session.sealed-deficient"),
        deficient_runtime->cases.front().definition.id, std::move(*store),
        QStringLiteral("engine.record-access.v1"), QStringLiteral("2026-08-11T10:02:00Z"),
        *deficient_resolved);
    QVERIFY(deficient_controller.has_value());
    const auto blocking = (*deficient_controller)->disclosures();
    QCOMPARE(blocking.size(), std::size_t{1});
    QCOMPARE(blocking.front().blocking_deficiencies.size(), std::size_t{2});
    QVERIFY(std::ranges::any_of(blocking.front().blocking_deficiencies, [](const auto& deficiency) {
        return deficiency.kind ==
               appellate::model::RecordDisclosureDeficiencyKind::MissingPublicMotion;
    }));
    QVERIFY(std::ranges::any_of(blocking.front().blocking_deficiencies, [](const auto& deficiency) {
        return deficiency.kind ==
               appellate::model::RecordDisclosureDeficiencyKind::MissingCertificate;
    }));
    const auto blocked = (*deficient_controller)
                             ->grant("example.disclosure.psr", "test.event.blocked-disclosure",
                                     QStringLiteral("2026-08-11T10:03:00Z"));
    QVERIFY(!blocked.has_value());
    QCOMPARE(blocked.error().code, appellate::app::SessionControllerErrorCode::EventCodecFailure);
    QCOMPARE((*deficient_controller)->snapshot().sequence, qint64{0});
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
    const auto lock_path = QDir(catalog_root).filePath(QStringLiteral(".install.lock"));
    const auto lock_bytes = readAll(lock_path);
    QVERIFY(!lock_bytes.isEmpty());
    const auto every_file = QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    const auto archives_before =
        QDir((*catalog)->archivesDirectory()).entryList({}, every_file, QDir::Name);
    const auto blobs_before =
        QDir((*catalog)->blobObjectsDirectory()).entryList({}, every_file, QDir::Name);
    const auto blocked =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("locked")),
                                   QStringLiteral("2026-08-11T00:00:01Z"));
    QVERIFY(!blocked.has_value());
    QCOMPARE(blocked.error().code, CatalogErrorCode::CatalogBusy);
    QCOMPARE(readAll(lock_path), lock_bytes);
    QCOMPARE(QDir((*catalog)->archivesDirectory()).entryList({}, every_file, QDir::Name),
             archives_before);
    QCOMPARE(QDir((*catalog)->blobObjectsDirectory()).entryList({}, every_file, QDir::Name),
             blobs_before);
    QVERIFY(
        !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"))));
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());
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
    {
        auto initialized = PackCatalog::open(catalog_root);
        QVERIFY(initialized.has_value());
    }

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    bool injected{};
    hooks.inject = [&injected](const appellate::packs::detail::CatalogObservation& observation) {
        if (!injected &&
            observation.operation == appellate::packs::detail::CatalogOperation::InstallArchive &&
            observation.event ==
                appellate::packs::detail::CatalogEvent::TransactionCommitAttempted) {
            injected = true;
            return appellate::packs::detail::CatalogInjectedAction::FailBefore;
        }
        return appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    auto catalog = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));

    const auto installed =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("rollback")),
                                   QStringLiteral("2026-08-11T00:00:01Z"));
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::CannotStoreArchive);
    QVERIFY(injected);
    const auto listed = (*catalog)->list();
    QVERIFY(listed.has_value());
    QVERIFY(listed->empty());

    const auto every_file = QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    QVERIFY(QDir((*catalog)->archivesDirectory()).entryList({}, every_file).empty());
    QVERIFY(QDir((*catalog)->blobObjectsDirectory()).entryList({}, every_file).empty());
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock"))));
    QVERIFY(
        !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"))));
    QVERIFY(report.remaining_ledger_paths.empty());
    QVERIFY(!report.residue_identity_ambiguous);

    const auto observed = [&report](appellate::packs::detail::CatalogEvent event) {
        return std::ranges::any_of(
            report.observations,
            [event](const appellate::packs::detail::CatalogObservation& observation) {
                return observation.operation ==
                           appellate::packs::detail::CatalogOperation::InstallArchive &&
                       observation.event == event;
            });
    };
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::TransactionCommitAttempted));
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::TransactionRolledBack));
    QVERIFY(!observed(appellate::packs::detail::CatalogEvent::TransactionCommitted));

    const auto retried =
        (*catalog)->installArchive(archivePath(temporary.path(), QStringLiteral("rollback")),
                                   QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
    QCOMPARE(retried->revision, *revision);
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::TransactionCommitted));
}

void PackDependencyResolutionTest::preservesCommittedInstallAfterReportedFinalizationFailure() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto revision = buildBlobArchive(
        temporary.path(), QStringLiteral("committed"), QStringLiteral("test.committed.root"),
        QByteArray("objects durable before a reported post-commit failure"));
    QVERIFY(revision.has_value());
    const auto catalog_root = QDir(temporary.path()).filePath(QStringLiteral("catalog"));
    {
        auto initialized = PackCatalog::open(catalog_root);
        QVERIFY(initialized.has_value());
    }

    auto scratch_context = appellate::packs::detail::acquireSecureScratchContext();
    QVERIFY2(scratch_context.has_value(),
             scratch_context ? "" : qPrintable(scratch_context.error().message));
    appellate::packs::detail::CatalogReport report;
    appellate::packs::detail::CatalogHooks hooks;
    hooks.report = &report;
    bool injected{};
    hooks.inject = [&injected](const appellate::packs::detail::CatalogObservation& observation) {
        if (!injected &&
            observation.operation == appellate::packs::detail::CatalogOperation::InstallArchive &&
            observation.event ==
                appellate::packs::detail::CatalogEvent::TransactionCommitAttempted) {
            injected = true;
            return appellate::packs::detail::CatalogInjectedAction::FailAfter;
        }
        return appellate::packs::detail::CatalogInjectedAction::Continue;
    };
    auto catalog = appellate::packs::detail::PackCatalogFactory::open(
        catalog_root, std::move(*scratch_context), std::move(hooks));
    QVERIFY2(catalog.has_value(), catalog ? "" : qPrintable(catalog.error().message));

    const auto original_time = QStringLiteral("2026-08-11T00:00:01Z");
    const auto installed = (*catalog)->installArchive(
        archivePath(temporary.path(), QStringLiteral("committed")), original_time);
    QVERIFY(!installed.has_value());
    QCOMPARE(installed.error().code, CatalogErrorCode::CannotStoreArchive);
    QVERIFY(injected);

    const auto observed = [&report](appellate::packs::detail::CatalogEvent event) {
        return std::ranges::any_of(
            report.observations,
            [event](const appellate::packs::detail::CatalogObservation& observation) {
                return observation.operation ==
                           appellate::packs::detail::CatalogOperation::InstallArchive &&
                       observation.event == event;
            });
    };
    QVERIFY(observed(appellate::packs::detail::CatalogEvent::TransactionCommitAttempted));
    QVERIFY(!observed(appellate::packs::detail::CatalogEvent::TransactionCommitted));
    QVERIFY(!observed(appellate::packs::detail::CatalogEvent::TransactionRolledBack));

    const auto archives_directory = (*catalog)->archivesDirectory();
    const auto blobs_directory = (*catalog)->blobObjectsDirectory();
    const auto archive_bytes = readAll(archivePath(temporary.path(), QStringLiteral("committed")));
    const auto blob_bytes = readAll(
        QDir(temporary.path()).filePath(QStringLiteral("sources/committed/objects/document.pdf")));
    QVERIFY(!archive_bytes.isEmpty());
    QVERIFY(!blob_bytes.isEmpty());
    const auto expected_archive =
        QString::fromLatin1(sha256(archive_bytes)) + QStringLiteral(".awpack");
    const auto expected_blob = QString::fromLatin1(sha256(blob_bytes));
    catalog->reset();

    auto reopened = PackCatalog::open(catalog_root);
    QVERIFY2(reopened.has_value(), reopened ? "" : qPrintable(reopened.error().message));
    const auto listed_after_error = (*reopened)->list();
    QVERIFY(listed_after_error.has_value());
    QCOMPARE(listed_after_error->size(), std::size_t{1});
    QCOMPARE(listed_after_error->front().revision, *revision);
    QCOMPARE(listed_after_error->front().installed_at_utc, original_time);

    const auto every_file = QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    QCOMPARE(QDir(archives_directory).entryList({}, every_file, QDir::Name),
             QStringList{expected_archive});
    QCOMPARE(QDir(blobs_directory).entryList({}, every_file, QDir::Name),
             QStringList{expected_blob});
    QCOMPARE(readAll(QDir(archives_directory).filePath(expected_archive)), archive_bytes);
    QCOMPARE(readAll(QDir(blobs_directory).filePath(expected_blob)), blob_bytes);
    QVERIFY(!QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock"))));
    QVERIFY(
        !QFileInfo::exists(QDir(catalog_root).filePath(QStringLiteral(".install.lock.rmlock"))));
    QVERIFY(report.remaining_ledger_paths.empty());
    QVERIFY(!report.residue_identity_ambiguous);

    const auto retried =
        (*reopened)->installArchive(archivePath(temporary.path(), QStringLiteral("committed")),
                                    QStringLiteral("2026-08-11T00:00:02Z"));
    QVERIFY2(retried.has_value(), retried ? "" : qPrintable(retried.error().message));
    QCOMPARE(retried->revision, *revision);
    QCOMPARE(retried->installed_at_utc, original_time);

    const auto listed_after_retry = (*reopened)->list();
    QVERIFY(listed_after_retry.has_value());
    QCOMPARE(listed_after_retry->size(), std::size_t{1});
    QCOMPARE(QDir(archives_directory).entryList({}, every_file, QDir::Name),
             QStringList{expected_archive});
    QCOMPARE(QDir(blobs_directory).entryList({}, every_file, QDir::Name),
             QStringList{expected_blob});
    QCOMPARE(readAll(QDir(archives_directory).filePath(expected_archive)), archive_bytes);
    QCOMPARE(readAll(QDir(blobs_directory).filePath(expected_blob)), blob_bytes);
}

} // namespace

QTEST_MAIN(PackDependencyResolutionTest)

#include "tst_pack_dependency_resolution.moc"
