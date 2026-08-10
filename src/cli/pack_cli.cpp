#include "pack_cli.hpp"

#include "appellate/model/pack_id.hpp"
#include "appellate/packs/error.hpp"
#include "appellate/packs/pack_archive.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_reader.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <array>
#include <string>
#include <utility>

static void initializeAppellatePackTemplateResources() {
    static const bool initialized = [] {
        Q_INIT_RESOURCE(templates);
        return true;
    }();
    static_cast<void>(initialized);
}

namespace appellate::cli {
namespace {

constexpr auto output_schema_version = 1;

[[nodiscard]] QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QByteArray encoded(QJsonObject object) {
    object.insert(QStringLiteral("schema_version"), output_schema_version);
    return QJsonDocument(std::move(object)).toJson(QJsonDocument::Compact) + '\n';
}

[[nodiscard]] RunResult success(QJsonObject object) {
    object.insert(QStringLiteral("status"), QStringLiteral("ok"));
    return RunResult{static_cast<int>(ExitCode::Success), encoded(std::move(object)), {}};
}

[[nodiscard]] RunResult failure(ExitCode exit_code, const QString& code, const QString& message,
                                const QString& command = {}) {
    QJsonObject object{
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
        {QStringLiteral("status"), QStringLiteral("error")},
    };
    if (!command.isEmpty()) {
        object.insert(QStringLiteral("command"), command);
    }
    return RunResult{static_cast<int>(exit_code), {}, encoded(std::move(object))};
}

[[nodiscard]] QString packErrorCode(packs::ErrorCode code) {
    switch (code) {
    case packs::ErrorCode::CannotRead:
        return QStringLiteral("cannot_read");
    case packs::ErrorCode::InvalidJson:
        return QStringLiteral("invalid_json");
    case packs::ErrorCode::InvalidManifest:
        return QStringLiteral("invalid_manifest");
    case packs::ErrorCode::UnsupportedSchema:
        return QStringLiteral("unsupported_schema");
    case packs::ErrorCode::UnsafePath:
        return QStringLiteral("unsafe_path");
    case packs::ErrorCode::DuplicateContentId:
        return QStringLiteral("duplicate_content_id");
    case packs::ErrorCode::DuplicateContentPath:
        return QStringLiteral("duplicate_content_path");
    case packs::ErrorCode::DuplicatePayloadId:
        return QStringLiteral("duplicate_payload_id");
    case packs::ErrorCode::UndeclaredFile:
        return QStringLiteral("undeclared_file");
    case packs::ErrorCode::ResourceTooLarge:
        return QStringLiteral("resource_too_large");
    case packs::ErrorCode::DigestMismatch:
        return QStringLiteral("digest_mismatch");
    case packs::ErrorCode::InvalidJudgeProfile:
        return QStringLiteral("invalid_judge_profile");
    case packs::ErrorCode::DuplicateJsonKey:
        return QStringLiteral("duplicate_json_key");
    case packs::ErrorCode::SchemaViolation:
        return QStringLiteral("schema_violation");
    case packs::ErrorCode::UnsupportedResourceKind:
        return QStringLiteral("unsupported_resource_kind");
    case packs::ErrorCode::CrossReferenceFailure:
        return QStringLiteral("cross_reference_failure");
    }
    return QStringLiteral("invalid_pack");
}

[[nodiscard]] QString catalogErrorCode(packs::CatalogErrorCode code) {
    switch (code) {
    case packs::CatalogErrorCode::InvalidConfiguration:
        return QStringLiteral("invalid_configuration");
    case packs::CatalogErrorCode::CannotOpen:
        return QStringLiteral("cannot_open_catalog");
    case packs::CatalogErrorCode::MigrationFailed:
        return QStringLiteral("catalog_migration_failed");
    case packs::CatalogErrorCode::ArchiveInvalid:
        return QStringLiteral("archive_invalid");
    case packs::CatalogErrorCode::ImmutableConflict:
        return QStringLiteral("immutable_conflict");
    case packs::CatalogErrorCode::MissingDependency:
        return QStringLiteral("missing_dependency");
    case packs::CatalogErrorCode::DependencyCycle:
        return QStringLiteral("dependency_cycle");
    case packs::CatalogErrorCode::CorruptCatalog:
        return QStringLiteral("corrupt_catalog");
    case packs::CatalogErrorCode::NotFound:
        return QStringLiteral("not_found");
    case packs::CatalogErrorCode::QueryFailed:
        return QStringLiteral("catalog_query_failed");
    case packs::CatalogErrorCode::CannotStoreArchive:
        return QStringLiteral("cannot_store_archive");
    case packs::CatalogErrorCode::CannotStoreBlob:
        return QStringLiteral("cannot_store_blob");
    }
    return QStringLiteral("catalog_error");
}

[[nodiscard]] QJsonObject revisionObject(const model::PackRevision& revision) {
    return QJsonObject{
        {QStringLiteral("digest"), fromUtf8(revision.digest)},
        {QStringLiteral("pack_id"), fromUtf8(revision.id.value)},
        {QStringLiteral("version"), fromUtf8(revision.version)},
    };
}

[[nodiscard]] QJsonObject installedObject(const packs::InstalledPack& installed) {
    QJsonArray dependencies;
    for (const auto& dependency : installed.dependencies) {
        dependencies.push_back(revisionObject(dependency.revision));
    }
    auto object = revisionObject(installed.revision);
    object.insert(QStringLiteral("archive_sha256"), installed.archive_sha256);
    object.insert(QStringLiteral("dependencies"), dependencies);
    object.insert(QStringLiteral("installed_at_utc"), installed.installed_at_utc);
    return object;
}

[[nodiscard]] QString usageMessage() {
    return QStringLiteral(
        "usage: appellate-pack validate <directory-or-awpack> | export <directory> "
        "<archive> | install <archive> <catalog> [--installed-at <canonical UTC>] | "
        "list <catalog> | template <new-destination>");
}

[[nodiscard]] bool isCanonicalUtc(const QString& value) {
    if (value.size() != 20 || !value.endsWith(u'Z')) {
        return false;
    }
    const auto parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.offsetFromUtc() == 0 &&
           parsed.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")) == value;
}

[[nodiscard]] QString currentCanonicalUtc() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'"));
}

[[nodiscard]] RunResult invalidArguments(const QString& message, const QString& command = {}) {
    return failure(ExitCode::InvalidArguments, QStringLiteral("invalid_arguments"),
                   message + QStringLiteral("; ") + usageMessage(), command);
}

[[nodiscard]] RunResult validateCommand(const QStringList& arguments) {
    constexpr auto command = "validate";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("validate requires exactly one source"),
                                QLatin1StringView(command));
    }
    const auto source = arguments.at(1);
    const auto is_directory = QFileInfo(source).isDir();
    const auto loaded = is_directory ? packs::PackReader::readDirectory(source)
                                     : packs::PackArchive::importArchive(source);
    if (!loaded) {
        return failure(ExitCode::InvalidPack, packErrorCode(loaded.error().code),
                       loaded.error().message, QLatin1StringView(command));
    }
    auto object = revisionObject(loaded->revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("blob_count"), static_cast<qint64>(loaded->blobs.size()));
    object.insert(QStringLiteral("resource_count"), static_cast<qint64>(loaded->resources.size()));
    object.insert(QStringLiteral("source_kind"),
                  is_directory ? QStringLiteral("directory") : QStringLiteral("archive"));
    return success(std::move(object));
}

[[nodiscard]] RunResult exportCommand(const QStringList& arguments) {
    constexpr auto command = "export";
    if (arguments.size() != 3) {
        return invalidArguments(QStringLiteral("export requires a source and destination"),
                                QLatin1StringView(command));
    }
    const auto exported = packs::PackArchive::exportDirectory(arguments.at(1), arguments.at(2));
    if (!exported) {
        return failure(ExitCode::InvalidPack, packErrorCode(exported.error().code),
                       exported.error().message, QLatin1StringView(command));
    }
    auto object = revisionObject(*exported);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    return success(std::move(object));
}

[[nodiscard]] RunResult installCommand(const QStringList& arguments) {
    constexpr auto command = "install";
    if (arguments.size() != 3 && arguments.size() != 5) {
        return invalidArguments(QStringLiteral("install received invalid arguments"),
                                QLatin1StringView(command));
    }
    QString installed_at = currentCanonicalUtc();
    if (arguments.size() == 5) {
        if (arguments.at(3) != QStringLiteral("--installed-at")) {
            return invalidArguments(QStringLiteral("install received an unknown option"),
                                    QLatin1StringView(command));
        }
        installed_at = arguments.at(4);
        if (!isCanonicalUtc(installed_at)) {
            return invalidArguments(
                QStringLiteral("--installed-at must be YYYY-MM-DDTHH:MM:SSZ in UTC"),
                QLatin1StringView(command));
        }
    }
    auto catalog = packs::PackCatalog::open(arguments.at(2));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    const auto installed = (*catalog)->installArchive(arguments.at(1), installed_at);
    if (!installed) {
        const auto exit_code = installed.error().code == packs::CatalogErrorCode::ArchiveInvalid
                                   ? ExitCode::InvalidPack
                                   : ExitCode::OperationFailed;
        return failure(exit_code, catalogErrorCode(installed.error().code),
                       installed.error().message, QLatin1StringView(command));
    }
    auto object = installedObject(*installed);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    return success(std::move(object));
}

[[nodiscard]] RunResult listCommand(const QStringList& arguments) {
    constexpr auto command = "list";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("list requires exactly one catalog"),
                                QLatin1StringView(command));
    }
    auto catalog = packs::PackCatalog::open(arguments.at(1));
    if (!catalog) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(catalog.error().code),
                       catalog.error().message, QLatin1StringView(command));
    }
    const auto listed = (*catalog)->list();
    if (!listed) {
        return failure(ExitCode::OperationFailed, catalogErrorCode(listed.error().code),
                       listed.error().message, QLatin1StringView(command));
    }
    QJsonArray packs_array;
    for (const auto& installed : *listed) {
        packs_array.push_back(installedObject(installed));
    }
    return success(QJsonObject{
        {QStringLiteral("command"), QLatin1StringView(command)},
        {QStringLiteral("packs"), packs_array},
    });
}

constexpr std::array<const char*, 14> template_members{
    "manifest.json",
    "objects/final-order.pdf",
    "resources/argument-config.json",
    "resources/authority-set.json",
    "resources/bench-configuration.json",
    "resources/case.json",
    "resources/court.json",
    "resources/filing-catalog.json",
    "resources/form.json",
    "resources/judge-profile.json",
    "resources/procedure-profile.json",
    "resources/realism-review.json",
    "resources/record.json",
    "resources/workflow.json",
};

[[nodiscard]] bool copyTemplateMember(const QString& member, const QString& staging_root,
                                      QString& error_message) {
    const auto source_path = QStringLiteral(":/appellate/templates/full-resource-pack/") + member;
    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        error_message = QStringLiteral("Cannot read embedded template member: %1").arg(member);
        return false;
    }
    const auto bytes = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        error_message =
            QStringLiteral("Cannot read complete embedded template member: %1").arg(member);
        return false;
    }
    const auto destination_path = QDir(staging_root).filePath(member);
    const auto parent = QFileInfo(destination_path).absolutePath();
    if (!QDir{}.mkpath(parent)) {
        error_message = QStringLiteral("Cannot create template member directory");
        return false;
    }
    QSaveFile destination(destination_path);
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly) ||
        destination.write(bytes) != static_cast<qint64>(bytes.size()) || !destination.commit()) {
        error_message = QStringLiteral("Cannot write template member: %1").arg(member);
        return false;
    }
    return true;
}

[[nodiscard]] RunResult templateCommand(const QStringList& arguments) {
    constexpr auto command = "template";
    if (arguments.size() != 2) {
        return invalidArguments(QStringLiteral("template requires exactly one destination"),
                                QLatin1StringView(command));
    }
    initializeAppellatePackTemplateResources();
    const auto requested = arguments.at(1);
    if (requested.trimmed().isEmpty()) {
        return invalidArguments(QStringLiteral("template destination cannot be empty"),
                                QLatin1StringView(command));
    }
    const auto destination = QFileInfo(requested).absoluteFilePath();
    const QFileInfo destination_info(destination);
    const auto parent_path = destination_info.absolutePath();
    const auto destination_name = destination_info.fileName();
    if (destination_name.isEmpty()) {
        return invalidArguments(QStringLiteral("template destination must name a directory"),
                                QLatin1StringView(command));
    }
    if (destination_info.exists() || destination_info.isSymLink()) {
        return failure(ExitCode::OperationFailed, QStringLiteral("destination_exists"),
                       QStringLiteral("Template destination already exists"),
                       QLatin1StringView(command));
    }
    if (!QDir{}.mkpath(parent_path)) {
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_create_destination"),
                       QStringLiteral("Cannot create template destination parent"),
                       QLatin1StringView(command));
    }

    const auto staging_pattern =
        QDir(parent_path)
            .filePath(QStringLiteral(".%1.appellate-template-XXXXXX").arg(destination_name));
    QTemporaryDir staging(staging_pattern);
    if (!staging.isValid()) {
        return failure(ExitCode::OperationFailed, QStringLiteral("cannot_create_staging"),
                       QStringLiteral("Cannot create private template staging directory"),
                       QLatin1StringView(command));
    }
    QString copy_error;
    for (const auto* member : template_members) {
        if (!copyTemplateMember(QLatin1StringView(member), staging.path(), copy_error)) {
            return failure(ExitCode::OperationFailed, QStringLiteral("cannot_extract_template"),
                           copy_error, QLatin1StringView(command));
        }
    }
    const auto validated = packs::PackReader::readDirectory(staging.path());
    if (!validated) {
        return failure(ExitCode::InvalidPack, packErrorCode(validated.error().code),
                       QStringLiteral("Embedded starter template failed validation: %1")
                           .arg(validated.error().message),
                       QLatin1StringView(command));
    }

    QDir parent(parent_path);
    const auto staging_name = QFileInfo(staging.path()).fileName();
    if (QFileInfo::exists(destination) || !parent.rename(staging_name, destination_name)) {
        const auto message = QFileInfo::exists(destination)
                                 ? QStringLiteral("Template destination already exists")
                                 : QStringLiteral("Cannot atomically install template directory");
        const auto code = QFileInfo::exists(destination)
                              ? QStringLiteral("destination_exists")
                              : QStringLiteral("cannot_create_destination");
        return failure(ExitCode::OperationFailed, code, message, QLatin1StringView(command));
    }
    staging.setAutoRemove(false);
    auto object = revisionObject(validated->revision);
    object.insert(QStringLiteral("command"), QLatin1StringView(command));
    object.insert(QStringLiteral("blob_count"), static_cast<qint64>(validated->blobs.size()));
    object.insert(QStringLiteral("resource_count"),
                  static_cast<qint64>(validated->resources.size()));
    return success(std::move(object));
}

} // namespace

RunResult runPackCli(const QStringList& arguments) {
    if (arguments.isEmpty()) {
        return invalidArguments(QStringLiteral("a command is required"));
    }
    const auto command = arguments.front();
    if (command == QStringLiteral("validate")) {
        return validateCommand(arguments);
    }
    if (command == QStringLiteral("export")) {
        return exportCommand(arguments);
    }
    if (command == QStringLiteral("install")) {
        return installCommand(arguments);
    }
    if (command == QStringLiteral("list")) {
        return listCommand(arguments);
    }
    if (command == QStringLiteral("template")) {
        return templateCommand(arguments);
    }
    return invalidArguments(QStringLiteral("unknown command: %1").arg(command), command);
}

} // namespace appellate::cli
