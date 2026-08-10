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
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace appellate::app {
namespace {

constexpr std::size_t maximum_record_entries = 4'096;
constexpr std::size_t maximum_record_dockets = 64;
constexpr std::size_t maximum_record_page_anchors = 32'768;
constexpr std::size_t maximum_entry_tags = 32;
constexpr std::uint32_t maximum_page_count = 10'000;
constexpr std::size_t hash_buffer_bytes = 64 * 1'024;

[[nodiscard]] auto fail(InstalledRecordErrorCode code, QString message)
    -> std::unexpected<InstalledRecordError> {
    return std::unexpected(InstalledRecordError{code, std::move(message)});
}

[[nodiscard]] QString asQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] std::optional<qsizetype> unicodeScalarCount(QStringView text) {
    qsizetype count = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto unit = text.at(index).unicode();
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (index + 1 >= text.size()) {
                return std::nullopt;
            }
            const auto low = text.at(index + 1).unicode();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return std::nullopt;
            }
            ++index;
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return std::nullopt;
        }
        ++count;
    }
    return count;
}

[[nodiscard]] bool isRoundTripText(const std::string& value, qsizetype maximum_size) {
    if (value.empty()) {
        return false;
    }
    const auto text = asQString(value);
    const auto count = unicodeScalarCount(text);
    return count.has_value() && *count <= maximum_size && !text.contains(QChar::Null) &&
           text.toUtf8().toStdString() == value;
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

[[nodiscard]] QString docketTypeName(packs::RuntimeDocketType type) {
    switch (type) {
    case packs::RuntimeDocketType::District:
        return QStringLiteral("district");
    case packs::RuntimeDocketType::Appellate:
        return QStringLiteral("appellate");
    case packs::RuntimeDocketType::Agency:
        return QStringLiteral("agency");
    case packs::RuntimeDocketType::Original:
        return QStringLiteral("original");
    }
    return {};
}

[[nodiscard]] QString relationshipName(packs::RuntimeRecordEntryRelationship relationship) {
    switch (relationship) {
    case packs::RuntimeRecordEntryRelationship::Attachment:
        return QStringLiteral("attachment");
    case packs::RuntimeRecordEntryRelationship::Amendment:
        return QStringLiteral("amendment");
    case packs::RuntimeRecordEntryRelationship::Supplement:
        return QStringLiteral("supplement");
    case packs::RuntimeRecordEntryRelationship::Component:
        return QStringLiteral("component");
    }
    return {};
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
    if (!isRoundTripText(runtime_case.record.id.value, 160) ||
        !isRoundTripText(runtime_case.record.caption, 512) ||
        runtime_case.record.dockets.size() > maximum_record_dockets ||
        runtime_case.record.docket_entries.empty() ||
        runtime_case.record.docket_entries.size() > maximum_record_entries ||
        runtime_case.record.page_anchors.size() > maximum_record_page_anchors) {
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
    const auto canonical_runtime = packs::loadRuntimePack(loaded_pack);
    if (!canonical_runtime) {
        return fail(InstalledRecordErrorCode::RuntimeMismatch,
                    QStringLiteral("Loaded pack cannot be rebuilt as a canonical runtime pack: %1")
                        .arg(asQString(canonical_runtime.error().message)));
    }
    if (*canonical_runtime != runtime_pack) {
        return fail(InstalledRecordErrorCode::RuntimeMismatch,
                    QStringLiteral(
                        "Supplied runtime pack differs from the exact loaded-pack projection"));
    }
    const auto& canonical_pack = *canonical_runtime;

    const auto identifies_selected_case = [&](const packs::RuntimeCase& runtime_case) {
        return runtime_case.definition.id == selected_case_id;
    };
    const auto case_matches =
        std::ranges::count_if(canonical_pack.cases, identifies_selected_case);
    if (case_matches == 0) {
        return fail(InstalledRecordErrorCode::MissingCase,
                    QStringLiteral("Selected runtime case does not exist"));
    }
    if (case_matches != 1) {
        return fail(InstalledRecordErrorCode::InvalidSelection,
                    QStringLiteral("Selected runtime case is not unique"));
    }
    const auto selected = std::ranges::find_if(canonical_pack.cases, identifies_selected_case);
    const auto& runtime_case = *selected;

    if (const auto identity = validateRecordIdentity(loaded_pack, runtime_case); !identity) {
        return std::unexpected(identity.error());
    }
    if (const auto valid = validateRuntimeRecord(runtime_case); !valid) {
        return std::unexpected(valid.error());
    }

    ui::RecordDefinition definition;
    definition.dockets.reserve(runtime_case.record.dockets.size());
    definition.documents.reserve(runtime_case.record.docket_entries.size());
    definition.docket.reserve(runtime_case.record.docket_entries.size());
    definition.anchors.reserve(runtime_case.record.page_anchors.size());
    std::vector<InstalledRecordAsset> assets;
    assets.reserve(runtime_case.record.docket_entries.size());

    std::unordered_map<std::string, const packs::RuntimeDocketDescriptor*> dockets_by_id;
    dockets_by_id.reserve(runtime_case.record.dockets.size());
    for (const auto& docket : runtime_case.record.dockets) {
        const auto type = docketTypeName(docket.type);
        if (!isRoundTripText(docket.id.value, 160) || type.isEmpty() ||
            !isRoundTripText(docket.court_ref, 240) ||
            !isRoundTripText(docket.public_docket_number, 120) ||
            !isRoundTripText(docket.caption, 512) ||
            (docket.court_id.has_value() &&
             !isRoundTripText(docket.court_id->value, 160)) ||
            !dockets_by_id.emplace(docket.id.value, &docket).second) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket descriptor metadata is invalid"));
        }
        definition.dockets.push_back(ui::RecordDocketDescriptor{
            asQString(docket.id.value), type,
            docket.court_id.has_value() ? asQString(docket.court_id->value) : QString{},
            asQString(docket.court_ref), asQString(docket.public_docket_number),
            asQString(docket.caption)});
    }

    std::unordered_map<std::string, const packs::RuntimeDocketEntry*> entries_by_id;
    entries_by_id.reserve(runtime_case.record.docket_entries.size());
    std::unordered_map<std::string, QStringList> citations_by_entry;
    citations_by_entry.reserve(runtime_case.record.page_anchors.size());
    for (const auto& entry : runtime_case.record.docket_entries) {
        if (!entries_by_id.emplace(entry.id.value, &entry).second) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket entry IDs are not unique"));
        }
    }
    std::unordered_map<std::string, std::string> parents;
    parents.reserve(runtime_case.record.docket_entries.size());
    for (const auto& entry : runtime_case.record.docket_entries) {
        if (entry.parent_entry_id.has_value() != entry.relationship.has_value()) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime parent links require an explicit relationship"));
        }
        if (!entry.parent_entry_id.has_value()) {
            continue;
        }
        const auto parent = entries_by_id.find(entry.parent_entry_id->value);
        if (parent == entries_by_id.end() || parent->second->id == entry.id ||
            parent->second->docket_id != entry.docket_id) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral(
                            "Runtime parent links must resolve within the same docket"));
        }
        parents.emplace(entry.id.value, entry.parent_entry_id->value);
    }
    std::unordered_set<std::string> resolved_parent_chains;
    for (const auto& entry : runtime_case.record.docket_entries) {
        std::unordered_set<std::string> chain;
        auto current = entry.id.value;
        while (parents.contains(current) && !resolved_parent_chains.contains(current)) {
            if (!chain.emplace(current).second) {
                return fail(InstalledRecordErrorCode::InvalidRecord,
                            QStringLiteral("Runtime docket parent graph contains a cycle"));
            }
            current = parents.at(current);
        }
        resolved_parent_chains.insert(chain.begin(), chain.end());
    }
    std::unordered_set<std::string> anchor_ids;
    anchor_ids.reserve(runtime_case.record.page_anchors.size());
    std::unordered_set<std::string> citation_labels;
    citation_labels.reserve(runtime_case.record.page_anchors.size());
    for (const auto& anchor : runtime_case.record.page_anchors) {
        const auto entry = entries_by_id.find(anchor.entry_id.value);
        if (!isRoundTripText(anchor.id.value, 160) ||
            !anchor_ids.emplace(anchor.id.value).second || entries_by_id.contains(anchor.id.value) ||
            entry == entries_by_id.end() || anchor.page_number == 0 ||
            anchor.page_number > entry->second->page_count ||
            (anchor.citation_label.has_value() &&
             (!isRoundTripText(*anchor.citation_label, 120) ||
              !citation_labels.emplace(*anchor.citation_label).second))) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime record page anchor is invalid"));
        }
        const auto citation = anchor.citation_label.has_value()
                                  ? asQString(*anchor.citation_label)
                                  : QString{};
        definition.anchors.push_back(ui::RecordPageAnchor{
            asQString(anchor.id.value), asQString(anchor.entry_id.value),
            static_cast<int>(anchor.page_number - 1), citation});
        if (!citation.isEmpty()) {
            citations_by_entry[anchor.entry_id.value].push_back(citation);
        }
    }

    for (const auto& entry : runtime_case.record.docket_entries) {
        const auto filed_on = asDate(entry.filed_on);
        if (!isRoundTripText(entry.id.value, 160) || !isRoundTripText(entry.title, 512) ||
            !isRoundTripText(entry.asset_path, 240) || !isLowercaseSha256(entry.asset_sha256) ||
            !filed_on.isValid() || entry.entry_number == 0 || entry.page_count == 0 ||
            entry.page_count > maximum_page_count ||
            entry.page_count > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket entry metadata is invalid"));
        }
        if ((entry.docket_id.has_value() &&
             (!isRoundTripText(entry.docket_id->value, 160) ||
              !dockets_by_id.contains(entry.docket_id->value))) ||
            (entry.entry_label.has_value() && !isRoundTripText(*entry.entry_label, 120)) ||
            (entry.actor.has_value() && !isRoundTripText(*entry.actor, 240)) ||
            (entry.description.has_value() &&
             !isRoundTripText(*entry.description, 4'096)) ||
            (entry.parent_entry_id.has_value() != entry.relationship.has_value()) ||
            (entry.parent_entry_id.has_value() &&
             (!entries_by_id.contains(entry.parent_entry_id->value) ||
              entry.parent_entry_id->value == entry.id.value))) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket entry relationships are invalid"));
        }
        QStringList tags;
        std::unordered_set<std::string> unique_tags;
        if (entry.tags.size() > maximum_entry_tags) {
            return fail(InstalledRecordErrorCode::InvalidRecord,
                        QStringLiteral("Runtime docket entry has too many tags"));
        }
        for (const auto& tag : entry.tags) {
            if (!isRoundTripText(tag, 64) || !unique_tags.emplace(tag).second) {
                return fail(InstalledRecordErrorCode::InvalidRecord,
                            QStringLiteral("Runtime docket entry tags are invalid"));
            }
            tags.push_back(asQString(tag));
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

        const auto materialized =
            catalog_.materializeBlob(canonical_pack.revision, entry.asset_path);
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
        QMap<QString, QString> metadata{
            {QStringLiteral("record_id"), asQString(runtime_case.record.id.value)},
            {QStringLiteral("entry_number"), QString::number(entry.entry_number)},
            {QStringLiteral("asset_sha256"), asQString(entry.asset_sha256)},
            {QStringLiteral("declared_page_count"), QString::number(entry.page_count)},
            {QStringLiteral("pack_id"), asQString(canonical_pack.revision.id.value)},
            {QStringLiteral("pack_version"), asQString(canonical_pack.revision.version)},
            {QStringLiteral("pack_revision_sha256"), asQString(canonical_pack.revision.digest)},
        };
        const auto docket = entry.docket_id.has_value()
                                ? dockets_by_id.at(entry.docket_id->value)
                                : nullptr;
        const auto docket_id =
            entry.docket_id.has_value() ? asQString(entry.docket_id->value) : QString{};
        const auto docket_label = docket != nullptr
                                      ? asQString(docket->public_docket_number)
                                      : QStringLiteral("Not specified by pack");
        const auto entry_label = entry.entry_label.has_value()
                                     ? asQString(*entry.entry_label)
                                     : QStringLiteral("Not specified by pack");
        const auto actor = entry.actor.has_value() ? asQString(*entry.actor)
                                                    : QStringLiteral("Not specified by pack");
        const auto description =
            entry.description.has_value() ? asQString(*entry.description)
                                          : QStringLiteral("Not specified by pack");
        const auto parent_id = entry.parent_entry_id.has_value()
                                   ? asQString(entry.parent_entry_id->value)
                                   : QString{};
        const auto relationship = entry.relationship.has_value()
                                      ? relationshipName(*entry.relationship)
                                      : QString{};
        metadata.insert(QStringLiteral("docket_id"),
                        docket_id.isEmpty() ? QStringLiteral("Not specified by pack") : docket_id);
        metadata.insert(QStringLiteral("docket_number"), docket_label);
        metadata.insert(QStringLiteral("entry_label"), entry_label);
        metadata.insert(QStringLiteral("actor"), actor);
        metadata.insert(QStringLiteral("description"), description);
        metadata.insert(QStringLiteral("tags"), tags.join(QStringLiteral(", ")));
        metadata.insert(QStringLiteral("parent_entry_id"),
                        parent_id.isEmpty() ? QStringLiteral("Not specified by pack") : parent_id);
        metadata.insert(QStringLiteral("relationship"),
                        relationship.isEmpty() ? QStringLiteral("Not specified by pack")
                                               : relationship);
        metadata.insert(QStringLiteral("page_anchor_citations"),
                        citations_by_entry[entry.id.value].join(QStringLiteral(", ")));
        if (docket != nullptr) {
            metadata.insert(QStringLiteral("docket_type"), docketTypeName(docket->type));
            metadata.insert(QStringLiteral("court_ref"), asQString(docket->court_ref));
            metadata.insert(QStringLiteral("docket_caption"), asQString(docket->caption));
            metadata.insert(QStringLiteral("docket_court_id"),
                            docket->court_id.has_value()
                                ? asQString(docket->court_id->value)
                                : QStringLiteral("Not specified by pack"));
        }
        definition.documents.push_back(
            ui::RecordDocument{entry_id, title, materialized->local_path, entry.sealed, metadata,
                               actual_page_count});
        definition.docket.push_back(ui::RecordDocketEntry{
            entry_id,
            filed_on,
            title,
            actor,
            description,
            entry_id,
            tags + QStringList{entry.sealed ? QStringLiteral("sealed")
                                            : QStringLiteral("unsealed")},
            metadata,
            docket_id,
            docket_label,
            entry_label,
            parent_id,
            relationship,
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

    return InstalledRecordLoad{canonical_pack.revision, runtime_case.definition.id,
                               runtime_case.record.id, std::move(definition), std::move(assets)};
}

} // namespace appellate::app
