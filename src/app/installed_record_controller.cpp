#include "installed_record_controller.hpp"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPdfDocument>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace appellate::app {
namespace {

constexpr std::size_t maximum_record_entries = 4'096;
constexpr std::uint32_t maximum_page_count = 10'000;
constexpr std::size_t hash_buffer_bytes = 64 * 1'024;

[[nodiscard]] auto fail(InstalledRecordErrorCode code, QString message)
    -> std::unexpected<InstalledRecordError> {
    return std::unexpected(InstalledRecordError{code, std::move(message)});
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] bool isRoundTripText(const std::string& value, qsizetype maximum_size) {
    if (value.empty() || value.size() > static_cast<std::size_t>(maximum_size)) {
        return false;
    }
    const auto text = asQString(value);
    return !text.contains(QChar::Null) && text.toUtf8().toStdString() == value;
}

[[nodiscard]] bool isLowercaseSha256(const std::string& value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] QDate asDate(model::LegalDate value) {
    return QDate(static_cast<int>(value.value.year()),
                 static_cast<int>(static_cast<unsigned>(value.value.month())),
                 static_cast<int>(static_cast<unsigned>(value.value.day())));
}

[[nodiscard]] bool isExactCatalogObject(const packs::PackCatalog& catalog,
                                        const packs::MaterializedBlob& blob) {
    const auto expected =
        QDir(catalog.blobObjectsDirectory()).filePath(asQString(blob.descriptor.sha256));
    return QDir::cleanPath(QFileInfo(blob.local_path).absoluteFilePath()) ==
           QDir::cleanPath(QFileInfo(expected).absoluteFilePath());
}

[[nodiscard]] auto verifyMaterializedBlob(const packs::PackCatalog& catalog,
                                          const packs::MaterializedBlob& blob)
    -> std::expected<void, InstalledRecordError> {
    if (blob.descriptor.media_type != "application/pdf" || blob.descriptor.byte_size == 0 ||
        !isLowercaseSha256(blob.descriptor.sha256) || !isExactCatalogObject(catalog, blob)) {
        return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                    QStringLiteral("Materialized record asset has an invalid descriptor or path"));
    }

    const QFileInfo before(blob.local_path);
    if (!before.isFile() || before.isSymLink() || before.size() < 0 ||
        static_cast<std::uint64_t>(before.size()) != blob.descriptor.byte_size) {
        return fail(
            InstalledRecordErrorCode::AssetVerificationFailure,
            QStringLiteral("Materialized record asset is missing, linked, or the wrong size"));
    }

    QFile input(blob.local_path);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                    QStringLiteral("Materialized record asset cannot be opened for verification"));
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, hash_buffer_bytes> buffer{};
    std::uint64_t total = 0;
    while (true) {
        const auto read = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (read < 0) {
            return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                        QStringLiteral("Materialized record asset cannot be read completely"));
        }
        if (read == 0) {
            break;
        }
        const auto chunk = static_cast<std::uint64_t>(read);
        if (chunk > blob.descriptor.byte_size || total > blob.descriptor.byte_size - chunk) {
            return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                        QStringLiteral("Materialized record asset exceeds its declared size"));
        }
        hash.addData(QByteArrayView(buffer.data(), read));
        total += chunk;
    }

    const QFileInfo after(blob.local_path);
    if (input.error() != QFileDevice::NoError || total != blob.descriptor.byte_size ||
        !after.isFile() || after.isSymLink() || after.size() != before.size() ||
        QString::fromLatin1(hash.result().toHex()).toStdString() != blob.descriptor.sha256) {
        return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                    QStringLiteral("Materialized record asset does not match its digest"));
    }
    return {};
}

[[nodiscard]] auto matchingResource(const packs::LoadedPack& pack, model::ResourceKind kind,
                                    const std::string& id)
    -> std::expected<const packs::ValidatedResource*, InstalledRecordError> {
    const auto matches = std::ranges::count_if(pack.resources, [&](const auto& resource) {
        return resource.descriptor.kind == kind && resource.descriptor.id == id;
    });
    if (matches != 1) {
        return fail(
            kind == model::ResourceKind::Record ? InstalledRecordErrorCode::OrphanRecord
                                                : InstalledRecordErrorCode::InvalidSelection,
            kind == model::ResourceKind::Record
                ? QStringLiteral("Selected case record is not uniquely declared by the pack")
                : QStringLiteral("Selected case is not uniquely declared by the pack"));
    }
    return &*std::ranges::find_if(pack.resources, [&](const auto& resource) {
        return resource.descriptor.kind == kind && resource.descriptor.id == id;
    });
}

[[nodiscard]] auto matchingBlob(const packs::LoadedPack& pack, const std::string& path)
    -> std::expected<const model::BlobDescriptor*, InstalledRecordError> {
    const auto matches = std::ranges::count(pack.blobs, path, &model::BlobDescriptor::path);
    if (matches != 1) {
        return fail(InstalledRecordErrorCode::MissingBlob,
                    QStringLiteral("Record asset is not uniquely declared as a pack blob"));
    }
    return &*std::ranges::find(pack.blobs, path, &model::BlobDescriptor::path);
}

[[nodiscard]] auto validateRecordIdentity(const packs::LoadedPack& loaded_pack,
                                          const packs::RuntimeCase& runtime_case)
    -> std::expected<void, InstalledRecordError> {
    const auto case_resource =
        matchingResource(loaded_pack, model::ResourceKind::Case, runtime_case.definition.id.value);
    if (!case_resource) {
        return std::unexpected(case_resource.error());
    }
    if ((*case_resource)->document.value(QStringLiteral("record_id")).toString().toStdString() !=
        runtime_case.record.id.value) {
        return fail(InstalledRecordErrorCode::OrphanRecord,
                    QStringLiteral("Selected case and runtime record are not bound to each other"));
    }
    const auto record_resource =
        matchingResource(loaded_pack, model::ResourceKind::Record, runtime_case.record.id.value);
    if (!record_resource) {
        return std::unexpected(record_resource.error());
    }
    if ((*record_resource)
            ->document.value(QStringLiteral("resource_id"))
            .toString()
            .toStdString() != runtime_case.record.id.value) {
        return fail(InstalledRecordErrorCode::OrphanRecord,
                    QStringLiteral("Selected runtime record has no matching authored resource"));
    }
    return {};
}

[[nodiscard]] auto validateRuntimeRecord(const packs::RuntimeCase& runtime_case)
    -> std::expected<void, InstalledRecordError> {
    if (!isRoundTripText(runtime_case.record.id.value, 512) ||
        !isRoundTripText(runtime_case.record.caption, 4'096) ||
        runtime_case.record.docket_entries.empty() ||
        runtime_case.record.docket_entries.size() > maximum_record_entries) {
        return fail(InstalledRecordErrorCode::InvalidRecord,
                    QStringLiteral("Runtime record identity, caption, or entry count is invalid"));
    }
    return {};
}

} // namespace

InstalledRecordController::InstalledRecordController(packs::PackCatalog& catalog,
                                                     ui::RecordWorkspace& workspace)
    : catalog_(catalog), workspace_(workspace) {}

std::expected<InstalledRecordLoad, InstalledRecordError>
InstalledRecordController::load(const packs::LoadedPack& loaded_pack,
                                const packs::RuntimePack& runtime_pack,
                                const model::CaseId& selected_case_id) {
    if (loaded_pack.revision != runtime_pack.revision) {
        return fail(InstalledRecordErrorCode::RevisionMismatch,
                    QStringLiteral("Loaded and runtime packs are different exact revisions"));
    }

    const auto identifies_selected_case = [&](const packs::RuntimeCase& runtime_case) {
        return runtime_case.definition.id == selected_case_id;
    };
    const auto case_matches = std::ranges::count_if(runtime_pack.cases, identifies_selected_case);
    if (case_matches == 0) {
        return fail(InstalledRecordErrorCode::MissingCase,
                    QStringLiteral("Selected runtime case does not exist"));
    }
    if (case_matches != 1) {
        return fail(InstalledRecordErrorCode::InvalidSelection,
                    QStringLiteral("Selected runtime case is not unique"));
    }
    const auto selected = std::ranges::find_if(runtime_pack.cases, identifies_selected_case);
    const auto& runtime_case = *selected;

    if (const auto identity = validateRecordIdentity(loaded_pack, runtime_case); !identity) {
        return std::unexpected(identity.error());
    }
    if (const auto valid = validateRuntimeRecord(runtime_case); !valid) {
        return std::unexpected(valid.error());
    }

    ui::RecordDefinition definition;
    definition.documents.reserve(runtime_case.record.docket_entries.size());
    definition.docket.reserve(runtime_case.record.docket_entries.size());
    std::vector<InstalledRecordAsset> assets;
    assets.reserve(runtime_case.record.docket_entries.size());

    for (const auto& entry : runtime_case.record.docket_entries) {
        const auto filed_on = asDate(entry.filed_on);
        if (!isRoundTripText(entry.id.value, 512) || !isRoundTripText(entry.title, 4'096) ||
            !isRoundTripText(entry.asset_path, 1'024) || !isLowercaseSha256(entry.asset_sha256) ||
            !filed_on.isValid() || entry.entry_number == 0 || entry.page_count == 0 ||
            entry.page_count > maximum_page_count ||
            entry.page_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket entry metadata is invalid"));
        }

        const auto declared = matchingBlob(loaded_pack, entry.asset_path);
        if (!declared) {
            return std::unexpected(declared.error());
        }
        if ((*declared)->sha256 != entry.asset_sha256) {
            return fail(InstalledRecordErrorCode::AssetDigestMismatch,
                        QStringLiteral("Runtime docket entry and pack blob digests differ"));
        }
        if ((*declared)->media_type != "application/pdf") {
            return fail(InstalledRecordErrorCode::AssetVerificationFailure,
                        QStringLiteral("Runtime record asset is not declared as a PDF"));
        }

        const auto materialized = catalog_.materializeBlob(runtime_pack.revision, entry.asset_path);
        if (!materialized) {
            return fail(InstalledRecordErrorCode::MaterializationFailure,
                        QStringLiteral("Installed record asset could not be materialized: %1")
                            .arg(materialized.error().message));
        }
        if (materialized->descriptor != **declared) {
            return fail(
                InstalledRecordErrorCode::AssetDigestMismatch,
                QStringLiteral("Loaded pack and installed catalog blob descriptors differ"));
        }
        if (const auto verified = verifyMaterializedBlob(catalog_, *materialized); !verified) {
            return std::unexpected(verified.error());
        }

        QPdfDocument pdf;
        const auto pdf_error = pdf.load(materialized->local_path);
        if (pdf_error != QPdfDocument::Error::None || pdf.status() != QPdfDocument::Status::Ready ||
            pdf.pageCount() <= 0) {
            return fail(InstalledRecordErrorCode::PdfLoadFailure,
                        QStringLiteral("Materialized record asset is not a readable PDF"));
        }
        const auto actual_page_count = pdf.pageCount();
        if (actual_page_count != static_cast<int>(entry.page_count)) {
            pdf.close();
            return fail(InstalledRecordErrorCode::PageCountMismatch,
                        QStringLiteral("Record PDF page count differs from its declaration"));
        }
        pdf.close();
        if (const auto verified = verifyMaterializedBlob(catalog_, *materialized); !verified) {
            return std::unexpected(verified.error());
        }

        const auto entry_id = asQString(entry.id.value);
        const auto title = asQString(entry.title);
        const QMap<QString, QString> metadata{
            {QStringLiteral("record_id"), asQString(runtime_case.record.id.value)},
            {QStringLiteral("entry_number"), QString::number(entry.entry_number)},
            {QStringLiteral("asset_sha256"), asQString(entry.asset_sha256)},
            {QStringLiteral("declared_page_count"), QString::number(entry.page_count)},
            {QStringLiteral("pack_id"), asQString(runtime_pack.revision.id.value)},
            {QStringLiteral("pack_version"), asQString(runtime_pack.revision.version)},
            {QStringLiteral("pack_revision_sha256"), asQString(runtime_pack.revision.digest)},
        };
        definition.documents.push_back(
            ui::RecordDocument{entry_id, title, materialized->local_path, entry.sealed, metadata});
        definition.docket.push_back(ui::RecordDocketEntry{
            entry_id,
            filed_on,
            title,
            QStringLiteral("Not specified by pack"),
            QStringLiteral("Docket entry %1 in %2; filing actor is not specified by schema v1")
                .arg(entry.entry_number)
                .arg(asQString(runtime_case.record.caption)),
            entry_id,
            {entry.sealed ? QStringLiteral("sealed") : QStringLiteral("unsealed")},
            metadata,
        });
        assets.push_back(InstalledRecordAsset{entry.id, materialized->descriptor,
                                              materialized->local_path, actual_page_count});
    }

    const auto workspace_result = workspace_.setRecord(definition);
    if (!workspace_result) {
        return fail(InstalledRecordErrorCode::WorkspaceFailure,
                    QStringLiteral("Native record workspace rejected the verified record: %1")
                        .arg(workspace_result.error().message));
    }

    return InstalledRecordLoad{runtime_pack.revision, runtime_case.definition.id,
                               runtime_case.record.id, std::move(definition), std::move(assets)};
}

} // namespace appellate::app
