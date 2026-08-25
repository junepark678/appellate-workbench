#include "realism_evidence.hpp"
#include "runtime_pack_internal.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/model/authority_ref.hpp"
#include "appellate/packs/pack_catalog.hpp"
#include "appellate/packs/pack_version.hpp"
#include "appellate/packs/realism_evidence_authoring.hpp"
#include "appellate/packs/schema_validator.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QString>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace appellate::packs {

namespace {

constexpr std::array dimension_names{
    "procedural_law", "deadlines_authority",   "record_consistency", "consequences",
    "oral_argument",  "bench_differentiation", "provenance",
};
constexpr qsizetype maximum_authoring_manifest_bytes = 1024 * 1024;
constexpr qsizetype maximum_authoring_review_bytes = 8 * 1024 * 1024;
constexpr qsizetype maximum_authoring_trace_count = 256;
constexpr qsizetype maximum_dimension_evidence_reference_count = 512;
constexpr std::size_t maximum_detached_subject_revision_count = 127;
constexpr std::size_t maximum_detached_subject_descriptor_count = 9'999;

[[nodiscard]] QString authoringEngineRevision() {
    return QString::fromLatin1(
        realism_evidence_authoring_engine_revision.data(),
        static_cast<qsizetype>(realism_evidence_authoring_engine_revision.size()));
}

[[nodiscard]] QString multiTraceAuthoringEngineRevision() {
    return QString::fromLatin1(
        realism_evidence_multi_trace_authoring_engine_revision.data(),
        static_cast<qsizetype>(realism_evidence_multi_trace_authoring_engine_revision.size()));
}

[[nodiscard]] QString detachedReviewEngineRevision() {
    return QString::fromLatin1(
        realism_evidence_detached_review_engine_revision.data(),
        static_cast<qsizetype>(realism_evidence_detached_review_engine_revision.size()));
}

[[nodiscard]] bool usesCodeOwnedTraceProfile(const QJsonObject& document) {
    const auto traces = document.value(QStringLiteral("evidence"))
                            .toObject()
                            .value(QStringLiteral("traces"))
                            .toArray();
    return std::ranges::any_of(traces, [](const QJsonValue& value) {
        const auto revision = value.toObject().value(QStringLiteral("engine_revision")).toString();
        return revision == authoringEngineRevision() ||
               revision == multiTraceAuthoringEngineRevision() ||
               revision == detachedReviewEngineRevision();
    });
}

[[nodiscard]] bool isUnicodeScalarSequence(QStringView value) {
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto unit = value.at(index).unicode();
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            if (++index >= value.size()) {
                return false;
            }
            const auto low = value.at(index).unicode();
            if (low < 0xDC00U || low > 0xDFFFU) {
                return false;
            }
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasOnlyUnicodeScalars(const QJsonValue& value) {
    if (value.isString()) {
        return isUnicodeScalarSequence(value.toString());
    }
    if (value.isArray()) {
        return std::ranges::all_of(value.toArray(), hasOnlyUnicodeScalars);
    }
    if (!value.isObject()) {
        return true;
    }
    const auto object = value.toObject();
    for (auto item = object.constBegin(); item != object.constEnd(); ++item) {
        if (!isUnicodeScalarSequence(item.key()) || !hasOnlyUnicodeScalars(item.value())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasExactKeys(const QJsonObject& object,
                                std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<qsizetype>(keys.size())) {
        return false;
    }
    return std::ranges::all_of(
        keys, [&object](const char* key) { return object.contains(QString::fromLatin1(key)); });
}

[[nodiscard]] bool isNamespacedId(const QString& value) {
    static const QRegularExpression pattern(
        QStringLiteral(R"(^[a-z0-9]+(?:[.-][a-z0-9]+)+(?:[-.][a-z0-9]+)*$)"));
    return value.size() >= 3 && value.size() <= 160 && pattern.match(value).hasMatch();
}

[[nodiscard]] bool isTrimStableUtf8Text(const QString& value, qsizetype maximum_bytes) {
    return !value.isEmpty() && value == value.trimmed() && isUnicodeScalarSequence(value) &&
           value.toUtf8().size() <= maximum_bytes;
}

[[nodiscard]] bool isCanonicalDate(const QString& value) {
    const auto parsed = QDate::fromString(value, Qt::ISODate);
    return parsed.isValid() && parsed.toString(Qt::ISODate) == value;
}

struct CapabilityBinding final {
    QString id;
    std::uint32_t version{};
    friend bool operator==(const CapabilityBinding&, const CapabilityBinding&) = default;
};

struct DependencyBinding final {
    QString pack_id;
    QString version;
    friend bool operator==(const DependencyBinding&, const DependencyBinding&) = default;
};

struct PackBinding final {
    QString pack_id;
    QString version;
    std::uint32_t manifest_schema_version{};
    std::vector<CapabilityBinding> capabilities;
    std::vector<DependencyBinding> dependencies;
    friend bool operator==(const PackBinding&, const PackBinding&) = default;
};

struct ResourceBinding final {
    QString evidence_id;
    QString owner_pack_id;
    QString owner_pack_version;
    QString resource_id;
    QString resource_kind;
    std::uint32_t schema_version{};
    QString path;
    QString sha256;
    friend bool operator==(const ResourceBinding&, const ResourceBinding&) = default;
};

struct BlobBinding final {
    QString evidence_id;
    QString owner_pack_id;
    QString owner_pack_version;
    QString path;
    QString media_type;
    std::uint64_t byte_size{};
    QString sha256;
    friend bool operator==(const BlobBinding&, const BlobBinding&) = default;
};

struct OwnedResource final {
    const LoadedPack* owner{};
    const ValidatedResource* resource{};
};

struct SubjectClosure final {
    const LoadedPack* case_owner{};
    const ValidatedResource* case_resource{};
    std::vector<const LoadedPack*> subject_packs_dependency_first;
    std::vector<PackBinding> pack_bindings;
    std::vector<ResourceBinding> resource_bindings;
    std::vector<BlobBinding> blob_bindings;
    std::map<std::string, OwnedResource> resources_by_id;
    std::map<std::string, BlobBinding> blobs_by_owner_path;
    QSet<QString> authority_ids;
};

[[nodiscard]] auto fail(const ValidatedResource& review, QString field, QString detail)
    -> std::unexpected<Error> {
    return std::unexpected(Error{
        ErrorCode::CrossReferenceFailure,
        QStringLiteral("Resource %1 has invalid realism evidence %2: %3")
            .arg(QString::fromStdString(review.descriptor.id), std::move(field), std::move(detail)),
    });
}

void addUint64(QCryptographicHash& hash, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (int index = 7; index >= 0; --index) {
        bytes.at(static_cast<std::size_t>(index)) = static_cast<char>(value & 0xffU);
        value >>= 8U;
    }
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
}

void addFrame(QCryptographicHash& hash, const QString& value) {
    const auto bytes = value.toUtf8();
    addUint64(hash, static_cast<std::uint64_t>(bytes.size()));
    hash.addData(QByteArrayView(bytes));
}

void addFrame(QCryptographicHash& hash, QByteArrayView value) {
    addUint64(hash, static_cast<std::uint64_t>(value.size()));
    hash.addData(value);
}

[[nodiscard]] QString kindName(model::ResourceKind kind) {
    switch (kind) {
    case model::ResourceKind::ArgumentConfig:
        return QStringLiteral("argument_config");
    case model::ResourceKind::AuthoritySet:
        return QStringLiteral("authority_set");
    case model::ResourceKind::BenchConfiguration:
        return QStringLiteral("bench_configuration");
    case model::ResourceKind::Case:
        return QStringLiteral("case");
    case model::ResourceKind::Court:
        return QStringLiteral("court");
    case model::ResourceKind::FilingCatalog:
        return QStringLiteral("filing_catalog");
    case model::ResourceKind::Form:
        return QStringLiteral("form");
    case model::ResourceKind::JudgeProfile:
        return QStringLiteral("judge_profile");
    case model::ResourceKind::ProcedureProfile:
        return QStringLiteral("procedure_profile");
    case model::ResourceKind::RealismReview:
        return QStringLiteral("realism_review");
    case model::ResourceKind::Record:
        return QStringLiteral("record");
    case model::ResourceKind::Workflow:
        return QStringLiteral("workflow");
    }
    return {};
}

[[nodiscard]] bool capabilityLess(const CapabilityBinding& left, const CapabilityBinding& right) {
    return std::tie(left.id, left.version) < std::tie(right.id, right.version);
}

[[nodiscard]] bool dependencyLess(const DependencyBinding& left, const DependencyBinding& right) {
    return std::tie(left.pack_id, left.version) < std::tie(right.pack_id, right.version);
}

[[nodiscard]] bool packLess(const PackBinding& left, const PackBinding& right) {
    return std::tie(left.pack_id, left.version) < std::tie(right.pack_id, right.version);
}

[[nodiscard]] bool resourceLess(const ResourceBinding& left, const ResourceBinding& right) {
    return std::tie(left.owner_pack_id, left.owner_pack_version, left.resource_id,
                    left.resource_kind, left.schema_version, left.path, left.sha256,
                    left.evidence_id) < std::tie(right.owner_pack_id, right.owner_pack_version,
                                                 right.resource_id, right.resource_kind,
                                                 right.schema_version, right.path, right.sha256,
                                                 right.evidence_id);
}

[[nodiscard]] bool blobLess(const BlobBinding& left, const BlobBinding& right) {
    return std::tie(left.owner_pack_id, left.owner_pack_version, left.path, left.media_type,
                    left.byte_size, left.sha256, left.evidence_id) <
           std::tie(right.owner_pack_id, right.owner_pack_version, right.path, right.media_type,
                    right.byte_size, right.sha256, right.evidence_id);
}

void addPackBinding(QCryptographicHash& hash, const PackBinding& binding) {
    addFrame(hash, binding.pack_id);
    addFrame(hash, binding.version);
    addUint64(hash, binding.manifest_schema_version);
    addUint64(hash, binding.capabilities.size());
    for (const auto& capability : binding.capabilities) {
        addFrame(hash, capability.id);
        addUint64(hash, capability.version);
    }
    addUint64(hash, binding.dependencies.size());
    for (const auto& dependency : binding.dependencies) {
        addFrame(hash, dependency.pack_id);
        addFrame(hash, dependency.version);
    }
}

void addResourceDescriptor(QCryptographicHash& hash, const ResourceBinding& binding) {
    addFrame(hash, binding.owner_pack_id);
    addFrame(hash, binding.owner_pack_version);
    addFrame(hash, binding.resource_id);
    addFrame(hash, binding.resource_kind);
    addUint64(hash, binding.schema_version);
    addFrame(hash, binding.path);
    addFrame(hash, binding.sha256);
}

void addResourceBinding(QCryptographicHash& hash, const ResourceBinding& binding) {
    addFrame(hash, binding.evidence_id);
    addResourceDescriptor(hash, binding);
}

void addBlobDescriptor(QCryptographicHash& hash, const BlobBinding& binding) {
    addFrame(hash, binding.owner_pack_id);
    addFrame(hash, binding.owner_pack_version);
    addFrame(hash, binding.path);
    addFrame(hash, binding.media_type);
    addUint64(hash, binding.byte_size);
    addFrame(hash, binding.sha256);
}

void addBlobBinding(QCryptographicHash& hash, const BlobBinding& binding) {
    addFrame(hash, binding.evidence_id);
    addBlobDescriptor(hash, binding);
}

[[nodiscard]] QString closureDigest(const QString& case_id, const std::vector<PackBinding>& packs,
                                    const std::vector<ResourceBinding>& resources,
                                    const std::vector<BlobBinding>& blobs) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-case-evidence-closure-v1"));
    addFrame(hash, case_id);
    addUint64(hash, packs.size());
    for (const auto& pack : packs) {
        addPackBinding(hash, pack);
    }
    addUint64(hash, resources.size());
    for (const auto& resource : resources) {
        addResourceBinding(hash, resource);
    }
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addBlobBinding(hash, blob);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString traceDigest(const QString& case_id, const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, case_id);
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toDouble()));
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toDouble()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operations = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operations.size()));
    for (const auto& operation : operations) {
        addFrame(hash, operation.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const model::WorkflowCommandHeader&
commandHeader(const model::WorkflowCommand& command) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowCommandHeader& { return concrete.header; },
        command);
}

[[nodiscard]] const model::WorkflowEventHeader& eventHeader(const model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> const model::WorkflowEventHeader& { return concrete.header; },
        event);
}

[[nodiscard]] std::expected<QByteArray, QString> canonicalBase64Bytes(const QJsonValue& value) {
    const auto encoded = value.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64(encoded);
    if (decoded.isEmpty() || decoded.toBase64() != encoded) {
        return std::unexpected(QStringLiteral("is not canonical base64"));
    }
    return decoded;
}

[[nodiscard]] std::expected<QJsonObject, Error>
normalizeExecutedTrace(const ValidatedResource& review, const QString& case_id, QJsonObject trace,
                       const RuntimeCase& runtime_case) {
    const auto engine_revision = trace.value(QStringLiteral("engine_revision")).toString();
    const auto uses_code_owned_profile = engine_revision == authoringEngineRevision() ||
                                         engine_revision == multiTraceAuthoringEngineRevision() ||
                                         engine_revision == detachedReviewEngineRevision();
    const auto validateDecodedJsonScalars =
        [&review, uses_code_owned_profile](QByteArrayView bytes,
                                           const QString& field) -> std::expected<void, Error> {
        if (!uses_code_owned_profile) {
            return {};
        }
        QJsonParseError parse_error;
        const auto decoded =
            QJsonDocument::fromJson(QByteArray(bytes.data(), bytes.size()), &parse_error);
        if (parse_error.error == QJsonParseError::NoError &&
            !hasOnlyUnicodeScalars(decoded.isObject() ? QJsonValue(decoded.object())
                                                      : QJsonValue(decoded.array()))) {
            return fail(review, field,
                        QStringLiteral("code-owned canonical journal JSON contains a non-scalar "
                                       "object key or string value"));
        }
        return {};
    };
    const auto journal_values = trace.value(QStringLiteral("journal")).toArray();
    const auto command_count = static_cast<std::uint64_t>(journal_values.size());

    QCryptographicHash journal_hash(QCryptographicHash::Sha256);
    addFrame(journal_hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(journal_hash, command_count);
    std::vector<model::WorkflowJournalEntry> journal;
    journal.reserve(static_cast<std::size_t>(journal_values.size()));
    QJsonArray executed_operation_ids;
    std::uint64_t event_count = 0;
    QString session_id;

    for (const auto& journal_value : journal_values) {
        const auto entry = journal_value.toObject();
        const auto command_bytes =
            canonicalBase64Bytes(entry.value(QStringLiteral("command_base64")));
        if (!command_bytes) {
            return fail(review, QStringLiteral("evidence/traces/journal/command_base64"),
                        command_bytes.error());
        }
        if (const auto scalars = validateDecodedJsonScalars(
                QByteArrayView(*command_bytes),
                QStringLiteral("evidence/traces/journal/command_base64"));
            !scalars) {
            return std::unexpected(scalars.error());
        }
        const auto command = storage::decodeWorkflowCommand(QByteArrayView(*command_bytes));
        if (!command) {
            return fail(review, QStringLiteral("evidence/traces/journal/command_base64"),
                        QStringLiteral("canonical workflow command cannot be decoded: %1")
                            .arg(command.error().message));
        }
        const auto reencoded_command = storage::encodeWorkflowCommand(*command);
        if (!reencoded_command || *reencoded_command != *command_bytes) {
            return fail(review, QStringLiteral("evidence/traces/journal/command_base64"),
                        QStringLiteral("workflow command bytes are not canonical"));
        }
        addFrame(journal_hash, QByteArrayView(*command_bytes));
        if (session_id.isEmpty()) {
            session_id = QString::fromStdString(commandHeader(*command).session_id);
        }

        const auto event_values = entry.value(QStringLiteral("events_base64")).toArray();
        addUint64(journal_hash, static_cast<std::uint64_t>(event_values.size()));
        std::vector<model::WorkflowEvent> events;
        events.reserve(static_cast<std::size_t>(event_values.size()));
        for (const auto& event_value : event_values) {
            const auto event_bytes = canonicalBase64Bytes(event_value);
            if (!event_bytes) {
                return fail(review, QStringLiteral("evidence/traces/journal/events_base64"),
                            event_bytes.error());
            }
            if (const auto scalars = validateDecodedJsonScalars(
                    QByteArrayView(*event_bytes),
                    QStringLiteral("evidence/traces/journal/events_base64"));
                !scalars) {
                return std::unexpected(scalars.error());
            }
            const auto event = storage::decodeWorkflowEvent(QByteArrayView(*event_bytes));
            if (!event) {
                return fail(review, QStringLiteral("evidence/traces/journal/events_base64"),
                            QStringLiteral("canonical workflow event cannot be decoded: %1")
                                .arg(event.error().message));
            }
            const auto reencoded_event = storage::encodeWorkflowEvent(*event);
            if (!reencoded_event || *reencoded_event != *event_bytes) {
                return fail(review, QStringLiteral("evidence/traces/journal/events_base64"),
                            QStringLiteral("workflow event bytes are not canonical"));
            }
            addFrame(journal_hash, QByteArrayView(*event_bytes));
            executed_operation_ids.push_back(
                QString::fromStdString(eventHeader(*event).operation_id.value));
            events.push_back(*event);
            ++event_count;
        }
        journal.push_back(model::WorkflowJournalEntry{*command, std::move(events)});
    }

    const auto computed_journal_sha256 = QString::fromLatin1(journal_hash.result().toHex());
    const auto requireDerived = [&](const QString& key,
                                    const QJsonValue& computed) -> std::expected<void, Error> {
        if (trace.contains(key) && trace.value(key) != computed) {
            return fail(review, QStringLiteral("evidence/traces/") + key,
                        QStringLiteral("supplied derived trace field is stale"));
        }
        trace.insert(key, computed);
        return {};
    };
    if (const auto derived = requireDerived(QStringLiteral("command_count"),
                                            static_cast<qint64>(journal_values.size()));
        !derived) {
        return std::unexpected(derived.error());
    }
    if (const auto derived =
            requireDerived(QStringLiteral("event_count"), static_cast<qint64>(event_count));
        !derived) {
        return std::unexpected(derived.error());
    }
    if (const auto derived =
            requireDerived(QStringLiteral("journal_sha256"), computed_journal_sha256);
        !derived) {
        return std::unexpected(derived.error());
    }
    if (const auto derived =
            requireDerived(QStringLiteral("operation_ids"), executed_operation_ids);
        !derived) {
        return std::unexpected(derived.error());
    }

    const model::WorkflowState initial_state{
        session_id.toStdString(),
        runtime_case.workflow.id,
        runtime_case.workflow.initial_stage_id,
        std::uint64_t{1},
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
    const auto replayed = engine::replayWorkflow(runtime_case.workflow, runtime_case.definition,
                                                 initial_state, journal);
    if (!replayed) {
        return fail(review, QStringLiteral("evidence/traces/journal"),
                    QStringLiteral("journal does not replay against the exact subject: %1")
                        .arg(QString::fromStdString(replayed.error().message)));
    }
    if (trace.value(QStringLiteral("workflow_id")).toString().toStdString() !=
        runtime_case.workflow.id.value) {
        return fail(review, QStringLiteral("evidence/traces/workflow_id"),
                    QStringLiteral("trace workflow differs from the exact reviewed case"));
    }
    if (const auto derived =
            requireDerived(QStringLiteral("terminal_stage_id"),
                           QString::fromStdString(replayed->current_stage_id.value));
        !derived) {
        return std::unexpected(derived.error());
    }
    const auto computed_digest = traceDigest(case_id, trace);
    if (const auto derived = requireDerived(QStringLiteral("digest"), computed_digest); !derived) {
        return std::unexpected(derived.error());
    }
    return trace;
}

[[nodiscard]] QString recordCheckDigest(const QString& case_id, const QJsonObject& check,
                                        const ResourceBinding& record,
                                        std::vector<BlobBinding> blobs) {
    std::ranges::sort(blobs, blobLess);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-record-check-evidence-v1"));
    addFrame(hash, case_id);
    addFrame(hash, check.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_id")).toString());
    addFrame(hash, check.value(QStringLiteral("record_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_kind")).toString());
    addResourceDescriptor(hash, record);
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addBlobDescriptor(hash, blob);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString revisionKey(const model::PackRevision& revision) {
    return QString::fromStdString(revision.id.value) + u'\n' +
           QString::fromStdString(revision.version) + u'\n' +
           QString::fromStdString(revision.digest);
}

[[nodiscard]] std::expected<SubjectClosure, Error>
buildSubjectClosure(const ValidatedResource& review, const QString& case_id,
                    std::span<const LoadedPack* const> all_packs) {
    std::map<std::string, OwnedResource> all_resources;
    const LoadedPack* case_owner = nullptr;
    const ValidatedResource* case_resource = nullptr;
    for (const auto* pack : all_packs) {
        for (const auto& resource : pack->resources) {
            const auto inserted =
                all_resources.emplace(resource.descriptor.id, OwnedResource{pack, &resource})
                    .second;
            if (!inserted) {
                return fail(review, QStringLiteral("case_id"),
                            QStringLiteral("the resolved closure contains duplicate resource IDs"));
            }
            if (resource.descriptor.id == case_id.toStdString() &&
                resource.descriptor.kind == model::ResourceKind::Case) {
                case_owner = pack;
                case_resource = &resource;
            }
        }
    }
    if (case_owner == nullptr || case_resource == nullptr) {
        return fail(review, QStringLiteral("case_id"),
                    QStringLiteral("the reviewed case does not resolve exactly once"));
    }

    const auto findPack = [all_packs](const model::PackRevision& revision) -> const LoadedPack* {
        const auto found = std::ranges::find_if(all_packs, [&revision](const LoadedPack* pack) {
            return pack != nullptr && pack->revision == revision;
        });
        return found == all_packs.end() ? nullptr : *found;
    };

    std::vector<const LoadedPack*> subject_packs;
    QSet<QString> visiting;
    QSet<QString> visited;
    std::function<std::expected<void, Error>(const LoadedPack&)> visit;
    visit = [&](const LoadedPack& pack) -> std::expected<void, Error> {
        const auto key = revisionKey(pack.revision);
        if (visited.contains(key)) {
            return {};
        }
        if (visiting.contains(key)) {
            return fail(review, QStringLiteral("closure_digest"),
                        QStringLiteral("the subject dependency topology contains a cycle"));
        }
        visiting.insert(key);
        for (const auto& dependency : pack.dependencies) {
            const auto* resolved = findPack(dependency.revision);
            if (resolved == nullptr) {
                return fail(review, QStringLiteral("closure_digest"),
                            QStringLiteral("an exact subject dependency is missing"));
            }
            const auto nested = visit(*resolved);
            if (!nested) {
                return nested;
            }
        }
        visiting.remove(key);
        visited.insert(key);
        subject_packs.push_back(&pack);
        return {};
    };
    const auto visited_subject = visit(*case_owner);
    if (!visited_subject) {
        return std::unexpected(visited_subject.error());
    }

    SubjectClosure closure;
    closure.case_owner = case_owner;
    closure.case_resource = case_resource;
    closure.subject_packs_dependency_first = subject_packs;
    for (const auto* pack : subject_packs) {
        PackBinding pack_binding{
            QString::fromStdString(pack->revision.id.value),
            QString::fromStdString(pack->revision.version),
            pack->manifest_schema_version,
            {},
            {},
        };
        pack_binding.capabilities.reserve(pack->required_capabilities.size());
        for (const auto& capability : pack->required_capabilities) {
            pack_binding.capabilities.push_back(
                CapabilityBinding{QString::fromStdString(capability.id), capability.version});
        }
        std::ranges::sort(pack_binding.capabilities, capabilityLess);
        pack_binding.dependencies.reserve(pack->dependencies.size());
        for (const auto& dependency : pack->dependencies) {
            pack_binding.dependencies.push_back(
                DependencyBinding{QString::fromStdString(dependency.revision.id.value),
                                  QString::fromStdString(dependency.revision.version)});
        }
        std::ranges::sort(pack_binding.dependencies, dependencyLess);
        closure.pack_bindings.push_back(std::move(pack_binding));

        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind == model::ResourceKind::RealismReview) {
                continue;
            }
            ResourceBinding binding{
                {},
                QString::fromStdString(pack->revision.id.value),
                QString::fromStdString(pack->revision.version),
                QString::fromStdString(resource.descriptor.id),
                kindName(resource.descriptor.kind),
                resource.descriptor.schema_version,
                QString::fromStdString(resource.descriptor.path),
                QString::fromStdString(resource.descriptor.sha256),
            };
            closure.resource_bindings.push_back(binding);
            closure.resources_by_id.emplace(resource.descriptor.id, OwnedResource{pack, &resource});
            if (resource.descriptor.kind == model::ResourceKind::AuthoritySet) {
                for (const auto& authority_value :
                     resource.document.value(QStringLiteral("authorities")).toArray()) {
                    closure.authority_ids.insert(authority_value.toObject()
                                                     .value(QStringLiteral("authority_id"))
                                                     .toString());
                }
            }
        }
        for (const auto& blob : pack->blobs) {
            BlobBinding binding{
                {},
                QString::fromStdString(pack->revision.id.value),
                QString::fromStdString(pack->revision.version),
                QString::fromStdString(blob.path),
                QString::fromStdString(blob.media_type),
                blob.byte_size,
                QString::fromStdString(blob.sha256),
            };
            closure.blob_bindings.push_back(binding);
            closure.blobs_by_owner_path.emplace(pack->revision.id.value + '\n' + blob.path,
                                                std::move(binding));
        }
    }
    std::ranges::sort(closure.pack_bindings, packLess);
    std::ranges::sort(closure.resource_bindings, resourceLess);
    std::ranges::sort(closure.blob_bindings, blobLess);
    return closure;
}

[[nodiscard]] QString evidenceId(const QString& category, std::initializer_list<QString> identity) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-realism-authoring-evidence-id-v1"));
    addFrame(hash, category);
    addUint64(hash, identity.size());
    for (const auto& value : identity) {
        addFrame(hash, value);
    }
    return QStringLiteral("workbench.evidence.%1.%2")
        .arg(category, QString::fromLatin1(hash.result().toHex()));
}

[[nodiscard]] QString recordCheckId(const QString& category, const QString& case_id) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-realism-authoring-record-check-id-v1"));
    addFrame(hash, category);
    addFrame(hash, case_id);
    auto id_category = category;
    id_category.replace(u'_', u'-');
    return QStringLiteral("workbench.check.%1.%2")
        .arg(id_category, QString::fromLatin1(hash.result().toHex()));
}

[[nodiscard]] QJsonObject packBindingObject(const PackBinding& binding) {
    QJsonArray capabilities;
    for (const auto& capability : binding.capabilities) {
        capabilities.push_back(QJsonObject{
            {QStringLiteral("id"), capability.id},
            {QStringLiteral("version"), static_cast<qint64>(capability.version)},
        });
    }
    QJsonArray dependencies;
    for (const auto& dependency : binding.dependencies) {
        dependencies.push_back(QJsonObject{
            {QStringLiteral("pack_id"), dependency.pack_id},
            {QStringLiteral("version"), dependency.version},
        });
    }
    return QJsonObject{
        {QStringLiteral("pack_id"), binding.pack_id},
        {QStringLiteral("version"), binding.version},
        {QStringLiteral("manifest_schema_version"),
         static_cast<qint64>(binding.manifest_schema_version)},
        {QStringLiteral("required_capabilities"), capabilities},
        {QStringLiteral("dependencies"), dependencies},
    };
}

[[nodiscard]] QJsonObject resourceBindingObject(const ResourceBinding& binding) {
    return QJsonObject{
        {QStringLiteral("evidence_id"), binding.evidence_id},
        {QStringLiteral("owner_pack_id"), binding.owner_pack_id},
        {QStringLiteral("owner_pack_version"), binding.owner_pack_version},
        {QStringLiteral("resource_id"), binding.resource_id},
        {QStringLiteral("resource_kind"), binding.resource_kind},
        {QStringLiteral("schema_version"), static_cast<qint64>(binding.schema_version)},
        {QStringLiteral("path"), binding.path},
        {QStringLiteral("sha256"), binding.sha256},
    };
}

[[nodiscard]] QJsonObject blobBindingObject(const BlobBinding& binding) {
    return QJsonObject{
        {QStringLiteral("evidence_id"), binding.evidence_id},
        {QStringLiteral("owner_pack_id"), binding.owner_pack_id},
        {QStringLiteral("owner_pack_version"), binding.owner_pack_version},
        {QStringLiteral("path"), binding.path},
        {QStringLiteral("media_type"), binding.media_type},
        {QStringLiteral("byte_size"), static_cast<qint64>(binding.byte_size)},
        {QStringLiteral("sha256"), binding.sha256},
    };
}

void collectAuthorityReferences(const QJsonValue& value, QSet<QString>& authority_ids) {
    if (value.isArray()) {
        for (const auto& child : value.toArray()) {
            collectAuthorityReferences(child, authority_ids);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }
    const auto object = value.toObject();
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        const auto& key = iterator.key();
        const auto& child = iterator.value();
        if ((key == QStringLiteral("authority_id") ||
             key == QStringLiteral("primary_authority_id") ||
             key == QStringLiteral("authorization_authority_id")) &&
            child.isString()) {
            authority_ids.insert(child.toString());
        } else if ((key == QStringLiteral("authority_ids") ||
                    key == QStringLiteral("supporting_authority_ids")) &&
                   child.isArray()) {
            for (const auto& authority : child.toArray()) {
                if (authority.isString()) {
                    authority_ids.insert(authority.toString());
                }
            }
        }
        collectAuthorityReferences(child, authority_ids);
    }
}

[[nodiscard]] QJsonArray sortedReferences(const QSet<QString>& references) {
    auto values = references.values();
    std::ranges::sort(values);
    QJsonArray result;
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

[[nodiscard]] QByteArray serializedObject(const QJsonObject& object) {
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) {
        bytes.push_back('\n');
    }
    return bytes;
}

[[nodiscard]] QString sha256(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] std::expected<QByteArray, QString> readAuthoringRegularFile(const QString& path,
                                                                          qsizetype maximum_bytes) {
#if defined(Q_OS_UNIX)
    const auto encoded = QFile::encodeName(path);
    const auto descriptor = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return std::unexpected(QStringLiteral("Cannot open authoring source %1: %2")
                                   .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
    }
    const auto close_descriptor = [&] { static_cast<void>(::close(descriptor)); };
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        before.st_size > static_cast<off_t>(maximum_bytes)) {
        close_descriptor();
        return std::unexpected(
            QStringLiteral("Authoring source is not a bounded regular file: %1").arg(path));
    }
    QByteArray bytes(static_cast<qsizetype>(before.st_size), Qt::Uninitialized);
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset,
                                  static_cast<std::size_t>(bytes.size() - offset));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close_descriptor();
            return std::unexpected(
                QStringLiteral("Authoring source changed while reading: %1").arg(path));
        }
        offset += static_cast<qsizetype>(count);
    }
    char extra{};
    ssize_t extra_count{};
    do {
        extra_count = ::read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    struct stat after{};
    const auto stable = extra_count == 0 && ::fstat(descriptor, &after) == 0 &&
                        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                        before.st_size == after.st_size && before.st_mtime == after.st_mtime &&
                        before.st_ctime == after.st_ctime;
    close_descriptor();
    if (!stable) {
        return std::unexpected(
            QStringLiteral("Authoring source changed or grew while reading: %1").arg(path));
    }
    return bytes;
#else
    static_cast<void>(path);
    static_cast<void>(maximum_bytes);
    return std::unexpected(
        QStringLiteral("This platform has no supported no-follow authoring source reader"));
#endif
}

[[nodiscard]] auto authoringFailure(RealismEvidenceAuthoringErrorCode code, QString message)
    -> std::unexpected<RealismEvidenceAuthoringError> {
    return std::unexpected(RealismEvidenceAuthoringError{code, std::move(message)});
}

[[nodiscard]] auto invalidPack(const Error& error)
    -> std::unexpected<RealismEvidenceAuthoringError> {
    return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack, error.message);
}

[[nodiscard]] auto catalogFailure(const CatalogError& error)
    -> std::unexpected<RealismEvidenceAuthoringError> {
    return authoringFailure(RealismEvidenceAuthoringErrorCode::CatalogFailure, error.message);
}

[[nodiscard]] const ValidatedResource*
findResource(const SubjectClosure& closure, const QString& resource_id, model::ResourceKind kind) {
    const auto found = closure.resources_by_id.find(resource_id.toStdString());
    if (found == closure.resources_by_id.end() || found->second.resource->descriptor.kind != kind) {
        return nullptr;
    }
    return found->second.resource;
}

struct AuthoringEvidenceProfile final {
    QString case_id;
    QString procedure_id;
    QString workflow_id;
    QString filing_catalog_id;
    QString court_id;
    QString record_id;
    QSet<QString> authority_set_ids;
    QSet<QString> argument_resource_ids;
    QSet<QString> bench_resource_ids;
    QSet<QString> judge_resource_ids;
    QSet<QString> case_authority_ids;
    QSet<QString> workflow_authority_ids;
    QSet<QString> deadline_authority_ids;
    QSet<QString> consequence_authority_ids;
    QSet<QString> filing_authority_ids;
    QSet<QString> record_authority_ids;
    QSet<QString> oral_authority_ids;
    QSet<QString> all_authority_ids;
};

[[nodiscard]] std::expected<AuthoringEvidenceProfile, QString>
buildAuthoringEvidenceProfile(const SubjectClosure& closure, const QString& case_id) {
    AuthoringEvidenceProfile profile;
    profile.case_id = case_id;
    profile.procedure_id =
        closure.case_resource->document.value(QStringLiteral("procedure_profile_id")).toString();
    profile.record_id =
        closure.case_resource->document.value(QStringLiteral("record_id")).toString();
    const auto* procedure =
        findResource(closure, profile.procedure_id, model::ResourceKind::ProcedureProfile);
    const auto* record = findResource(closure, profile.record_id, model::ResourceKind::Record);
    if (procedure == nullptr || record == nullptr) {
        return std::unexpected(QStringLiteral("The reviewed case procedure or record is absent"));
    }

    profile.workflow_id = procedure->document.value(QStringLiteral("workflow_id")).toString();
    profile.filing_catalog_id =
        procedure->document.value(QStringLiteral("filing_catalog_id")).toString();
    profile.court_id = procedure->document.value(QStringLiteral("court_id")).toString();
    const auto* workflow =
        findResource(closure, profile.workflow_id, model::ResourceKind::Workflow);
    const auto* filing_catalog =
        findResource(closure, profile.filing_catalog_id, model::ResourceKind::FilingCatalog);
    if (workflow == nullptr || filing_catalog == nullptr ||
        findResource(closure, profile.court_id, model::ResourceKind::Court) == nullptr) {
        return std::unexpected(QStringLiteral("The reviewed procedure resources are incomplete"));
    }

    QSet<QString> scoped_authority_ids;
    for (const auto& value :
         procedure->document.value(QStringLiteral("authority_set_ids")).toArray()) {
        const auto set_id = value.toString();
        const auto* authority_set =
            findResource(closure, set_id, model::ResourceKind::AuthoritySet);
        if (authority_set == nullptr) {
            return std::unexpected(QStringLiteral("A procedure authority set is absent"));
        }
        profile.authority_set_ids.insert(set_id);
        for (const auto& authority_value :
             authority_set->document.value(QStringLiteral("authorities")).toArray()) {
            scoped_authority_ids.insert(
                authority_value.toObject().value(QStringLiteral("authority_id")).toString());
        }
    }

    collectAuthorityReferences(closure.case_resource->document, profile.case_authority_ids);
    collectAuthorityReferences(record->document, profile.record_authority_ids);

    QSet<QString> routed_filing_ids;
    for (const auto& route_value :
         workflow->document.value(QStringLiteral("filing_routes")).toArray()) {
        routed_filing_ids.insert(
            route_value.toObject().value(QStringLiteral("filing_type_id")).toString());
    }
    for (const auto& filing_value :
         filing_catalog->document.value(QStringLiteral("filings")).toArray()) {
        const auto filing = filing_value.toObject();
        if (routed_filing_ids.contains(filing.value(QStringLiteral("filing_id")).toString())) {
            collectAuthorityReferences(filing, profile.filing_authority_ids);
        }
    }

    static const QSet<QString> consequence_opcodes{
        QStringLiteral("enter_order"),
        QStringLiteral("set_sealed"),
        QStringLiteral("issue_judgment"),
        QStringLiteral("issue_mandate"),
    };
    for (const auto& operation_value :
         workflow->document.value(QStringLiteral("operations")).toArray()) {
        const auto operation = operation_value.toObject();
        QSet<QString> operation_authorities;
        collectAuthorityReferences(operation, operation_authorities);
        profile.workflow_authority_ids.unite(operation_authorities);
        const auto opcode = operation.value(QStringLiteral("opcode")).toString();
        if (opcode == QStringLiteral("calculate_deadline")) {
            profile.deadline_authority_ids.unite(operation_authorities);
        }
        if (consequence_opcodes.contains(opcode)) {
            profile.consequence_authority_ids.unite(operation_authorities);
        }
    }

    for (const auto& [resource_id, owned] : closure.resources_by_id) {
        static_cast<void>(resource_id);
        if (owned.resource->descriptor.kind != model::ResourceKind::ArgumentConfig ||
            owned.resource->document.value(QStringLiteral("case_id")).toString() != case_id) {
            continue;
        }
        const auto argument_id = QString::fromStdString(owned.resource->descriptor.id);
        profile.argument_resource_ids.insert(argument_id);
        collectAuthorityReferences(owned.resource->document, profile.oral_authority_ids);
        const auto bench_id =
            owned.resource->document.value(QStringLiteral("bench_configuration_id")).toString();
        const auto* bench =
            findResource(closure, bench_id, model::ResourceKind::BenchConfiguration);
        if (bench == nullptr) {
            return std::unexpected(QStringLiteral("A case argument bench is absent"));
        }
        profile.bench_resource_ids.insert(bench_id);
        for (const auto& seat_value : bench->document.value(QStringLiteral("seats")).toArray()) {
            const auto judge_id =
                seat_value.toObject().value(QStringLiteral("profile_id")).toString();
            if (findResource(closure, judge_id, model::ResourceKind::JudgeProfile) == nullptr) {
                return std::unexpected(QStringLiteral("A case argument judge profile is absent"));
            }
            profile.judge_resource_ids.insert(judge_id);
        }
    }

    profile.all_authority_ids = profile.case_authority_ids;
    profile.all_authority_ids.unite(profile.workflow_authority_ids);
    profile.all_authority_ids.unite(profile.deadline_authority_ids);
    profile.all_authority_ids.unite(profile.consequence_authority_ids);
    profile.all_authority_ids.unite(profile.filing_authority_ids);
    profile.all_authority_ids.unite(profile.record_authority_ids);
    profile.all_authority_ids.unite(profile.oral_authority_ids);
    for (const auto& authority_id : profile.all_authority_ids) {
        if (!scoped_authority_ids.contains(authority_id) ||
            !closure.authority_ids.contains(authority_id)) {
            return std::unexpected(
                QStringLiteral("Canonical authority %1 is outside the selected procedure scope")
                    .arg(authority_id));
        }
    }
    if (profile.all_authority_ids.isEmpty()) {
        return std::unexpected(
            QStringLiteral("The selected case procedure has no referenced canonical authority"));
    }
    return profile;
}

struct AuthoringEvidenceBindings final {
    QHash<QString, QString> resources_by_id;
    QHash<QString, QString> authorities_by_id;
    QSet<QString> record_blob_refs;
    QSet<QString> record_check_refs;
    QSet<QString> trace_refs;
};

[[nodiscard]] QHash<QString, QSet<QString>>
authoringDimensionGroups(const AuthoringEvidenceProfile& profile,
                         const AuthoringEvidenceBindings& bindings) {
    const auto resourceRefs = [&](const QSet<QString>& resource_ids) {
        QSet<QString> refs;
        for (const auto& resource_id : resource_ids) {
            const auto found = bindings.resources_by_id.constFind(resource_id);
            if (found != bindings.resources_by_id.constEnd()) {
                refs.insert(*found);
            }
        }
        return refs;
    };
    const auto authorityRefs = [&](QSet<QString> authority_ids) {
        QSet<QString> refs;
        for (const auto& authority_id : authority_ids) {
            const auto found = bindings.authorities_by_id.constFind(authority_id);
            if (found != bindings.authorities_by_id.constEnd()) {
                refs.insert(*found);
            }
        }
        return refs;
    };
    const auto oneResourceRef = [&](const QString& resource_id) {
        return resourceRefs(QSet<QString>{resource_id});
    };
    const auto authority_set_refs = resourceRefs(profile.authority_set_ids);

    auto procedural_authorities = profile.case_authority_ids;
    procedural_authorities.unite(profile.workflow_authority_ids);
    procedural_authorities.unite(profile.filing_authority_ids);
    auto procedural = oneResourceRef(profile.case_id);
    procedural.unite(oneResourceRef(profile.procedure_id));
    procedural.unite(oneResourceRef(profile.workflow_id));
    procedural.unite(oneResourceRef(profile.filing_catalog_id));
    procedural.unite(authority_set_refs);
    procedural.unite(authorityRefs(procedural_authorities));
    procedural.unite(bindings.trace_refs);

    auto deadlines = oneResourceRef(profile.procedure_id);
    deadlines.unite(oneResourceRef(profile.workflow_id));
    deadlines.unite(oneResourceRef(profile.court_id));
    deadlines.unite(authority_set_refs);
    deadlines.unite(authorityRefs(profile.deadline_authority_ids));
    deadlines.unite(bindings.trace_refs);

    auto record_consistency = oneResourceRef(profile.record_id);
    record_consistency.unite(bindings.record_blob_refs);
    record_consistency.unite(bindings.record_check_refs);

    auto consequence_authorities = profile.case_authority_ids;
    consequence_authorities.unite(profile.consequence_authority_ids);
    auto consequences = oneResourceRef(profile.case_id);
    consequences.unite(oneResourceRef(profile.workflow_id));
    consequences.unite(authorityRefs(consequence_authorities));
    consequences.unite(bindings.trace_refs);

    auto oral_argument = resourceRefs(profile.argument_resource_ids);
    oral_argument.unite(resourceRefs(profile.bench_resource_ids));
    oral_argument.unite(resourceRefs(profile.judge_resource_ids));
    oral_argument.unite(oneResourceRef(profile.record_id));
    oral_argument.unite(authorityRefs(profile.oral_authority_ids));

    auto bench_differentiation = resourceRefs(profile.bench_resource_ids);
    bench_differentiation.unite(resourceRefs(profile.judge_resource_ids));

    auto provenance = authority_set_refs;
    provenance.unite(authorityRefs(profile.all_authority_ids));
    provenance.unite(oneResourceRef(profile.record_id));
    provenance.unite(bindings.record_blob_refs);
    provenance.unite(bindings.record_check_refs);

    return QHash<QString, QSet<QString>>{
        {QStringLiteral("procedural_law"), procedural},
        {QStringLiteral("deadlines_authority"), deadlines},
        {QStringLiteral("record_consistency"), record_consistency},
        {QStringLiteral("consequences"), consequences},
        {QStringLiteral("oral_argument"), oral_argument},
        {QStringLiteral("bench_differentiation"), bench_differentiation},
        {QStringLiteral("provenance"), provenance},
    };
}

[[nodiscard]] std::expected<void, Error>
validateCodeOwnedScorePrerequisites(const ValidatedResource& review,
                                    const AuthoringEvidenceProfile& profile) {
    const auto dimensions = review.document.value(QStringLiteral("dimensions")).toObject();
    if (dimensions.value(QStringLiteral("oral_argument")).toInt() > 0 &&
        profile.argument_resource_ids.isEmpty()) {
        return fail(review, QStringLiteral("dimensions/oral_argument"),
                    QStringLiteral("nonzero code-owned oral-argument evidence requires a "
                                   "case-targeted argument configuration"));
    }
    if (dimensions.value(QStringLiteral("bench_differentiation")).toInt() > 0 &&
        (profile.bench_resource_ids.isEmpty() || profile.judge_resource_ids.isEmpty())) {
        return fail(review, QStringLiteral("dimensions/bench_differentiation"),
                    QStringLiteral("nonzero code-owned bench differentiation requires referenced "
                                   "bench and judge profiles"));
    }
    return {};
}

[[nodiscard]] std::expected<void, Error>
validateDetachedHumanFields(const ValidatedResource& review) {
    const auto reference = review.document.value(QStringLiteral("reviewer_reference"));
    if (!reference.isString() || !isTrimStableUtf8Text(reference.toString(), 512)) {
        return fail(review, QStringLiteral("reviewer_reference"),
                    QStringLiteral("detached reviewer reference must be trim-stable UTF-8 text "
                                   "within 512 bytes"));
    }

    const auto reviewer_value = review.document.value(QStringLiteral("reviewer"));
    if (!reviewer_value.isObject()) {
        return fail(review, QStringLiteral("reviewer"),
                    QStringLiteral("detached review requires reviewer metadata"));
    }
    const auto reviewer = reviewer_value.toObject();
    const auto has_affiliation = reviewer.contains(QStringLiteral("affiliation"));
    if ((!has_affiliation &&
         !hasExactKeys(reviewer, {"reviewer_id", "display_name", "qualification"})) ||
        (has_affiliation && !hasExactKeys(reviewer, {"reviewer_id", "display_name", "qualification",
                                                     "affiliation"}))) {
        return fail(review, QStringLiteral("reviewer"),
                    QStringLiteral("detached reviewer metadata has an invalid closed shape"));
    }
    const auto reviewer_id = reviewer.value(QStringLiteral("reviewer_id"));
    const auto display_name = reviewer.value(QStringLiteral("display_name"));
    const auto qualification = reviewer.value(QStringLiteral("qualification"));
    if (!reviewer_id.isString() || !isNamespacedId(reviewer_id.toString()) ||
        reviewer_id.toString().toUtf8().size() > 160 || !display_name.isString() ||
        !isTrimStableUtf8Text(display_name.toString(), 240) || !qualification.isString() ||
        !isTrimStableUtf8Text(qualification.toString(), 1'024) ||
        (has_affiliation &&
         (!reviewer.value(QStringLiteral("affiliation")).isString() ||
          !isTrimStableUtf8Text(reviewer.value(QStringLiteral("affiliation")).toString(), 240)))) {
        return fail(review, QStringLiteral("reviewer"),
                    QStringLiteral("detached reviewer metadata violates its persistent text "
                                   "contract"));
    }

    const auto uncertainties_value = review.document.value(QStringLiteral("known_uncertainty"));
    if (!uncertainties_value.isArray() || uncertainties_value.toArray().size() > 256) {
        return fail(review, QStringLiteral("known_uncertainty"),
                    QStringLiteral("detached uncertainties must contain at most 256 items"));
    }
    QSet<QString> uncertainty_ids;
    for (const auto& value : uncertainties_value.toArray()) {
        if (!value.isObject()) {
            return fail(review, QStringLiteral("known_uncertainty"),
                        QStringLiteral("detached uncertainties must be typed objects"));
        }
        const auto uncertainty = value.toObject();
        const auto blocking_value = uncertainty.value(QStringLiteral("blocking"));
        const auto blocking = blocking_value.isBool() && blocking_value.toBool();
        const auto has_remediation = uncertainty.contains(QStringLiteral("remediation_issue"));
        if (!blocking_value.isBool() ||
            (!has_remediation &&
             !hasExactKeys(uncertainty, {"uncertainty_id", "summary", "blocking"})) ||
            (has_remediation && !hasExactKeys(uncertainty, {"uncertainty_id", "summary", "blocking",
                                                            "remediation_issue"})) ||
            blocking != has_remediation) {
            return fail(review, QStringLiteral("known_uncertainty"),
                        QStringLiteral("detached uncertainty has an invalid closed shape"));
        }
        const auto uncertainty_id = uncertainty.value(QStringLiteral("uncertainty_id"));
        const auto summary = uncertainty.value(QStringLiteral("summary"));
        if (!uncertainty_id.isString() || !isNamespacedId(uncertainty_id.toString()) ||
            uncertainty_id.toString().toUtf8().size() > 160 ||
            uncertainty_ids.contains(uncertainty_id.toString()) || !summary.isString() ||
            !isTrimStableUtf8Text(summary.toString(), 2'048)) {
            return fail(review, QStringLiteral("known_uncertainty"),
                        QStringLiteral("detached uncertainty ID or summary is invalid"));
        }
        uncertainty_ids.insert(uncertainty_id.toString());
        if (blocking) {
            const auto remediation = uncertainty.value(QStringLiteral("remediation_issue"));
            const auto remediation_bytes = remediation.toString().toUtf8();
            if (!remediation.isString() || !isUnicodeScalarSequence(remediation.toString()) ||
                !model::isCanonicalAuthoritySourceUrl(
                    std::string_view(remediation_bytes.constData(),
                                     static_cast<std::size_t>(remediation_bytes.size())))) {
                return fail(review, QStringLiteral("known_uncertainty/remediation_issue"),
                            QStringLiteral("detached remediation issue is not a canonical HTTPS "
                                           "authority URL"));
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, Error>
validateDetachedProfile(const ValidatedResource& review, const LoadedPack& review_owner,
                        const SubjectClosure& closure,
                        const QHash<QString, QSet<QString>>& complete_dimension_groups) {
    const auto exact_capabilities =
        review_owner.required_capabilities.size() == 2 &&
        review_owner.required_capabilities.at(0) ==
            model::RequiredCapability{"workbench.pack.declarative-resources", 2} &&
        review_owner.required_capabilities.at(1) ==
            model::RequiredCapability{"workbench.pack.realism-evidence", 1};
    const auto exact_review_owner =
        review_owner.manifest_schema_version == 2 && review_owner.resources.size() == 1 &&
        &review_owner.resources.front() == &review &&
        review.descriptor.kind == model::ResourceKind::RealismReview &&
        review.descriptor.schema_version == 2 &&
        review.descriptor.path == "resources/realism-review.json" &&
        review.descriptor.id ==
            review.document.value(QStringLiteral("resource_id")).toString().toStdString() &&
        review.descriptor.sha256 ==
            sha256(QByteArrayView(serializedObject(review.document))).toStdString() &&
        review_owner.dependencies.size() == 1 &&
        review_owner.dependencies.front().revision == closure.case_owner->revision &&
        review_owner.blobs.empty() && exact_capabilities;
    if (!exact_review_owner) {
        return fail(review, QStringLiteral("review_state"),
                    QStringLiteral("detached profile requires the exact review-only owner shape"));
    }

    if (closure.subject_packs_dependency_first.size() > maximum_detached_subject_revision_count) {
        return fail(review, QStringLiteral("evidence/packs"),
                    QStringLiteral("detached subject closure leaves no revision headroom"));
    }
    std::size_t descriptor_count = 0;
    for (const auto* pack : closure.subject_packs_dependency_first) {
        const auto resource_count = pack->resources.size();
        const auto blob_count = pack->blobs.size();
        if (resource_count > maximum_detached_subject_descriptor_count - descriptor_count) {
            return fail(review, QStringLiteral("evidence/resources"),
                        QStringLiteral("detached subject closure leaves no descriptor headroom"));
        }
        descriptor_count += resource_count;
        if (blob_count > maximum_detached_subject_descriptor_count - descriptor_count) {
            return fail(review, QStringLiteral("evidence/blobs"),
                        QStringLiteral("detached subject closure leaves no descriptor headroom"));
        }
        descriptor_count += blob_count;
    }

    std::vector<const ValidatedResource*> source_reviews;
    const auto case_id = review.document.value(QStringLiteral("case_id")).toString();
    for (const auto& resource : closure.case_owner->resources) {
        if (resource.descriptor.kind == model::ResourceKind::RealismReview &&
            resource.descriptor.schema_version == 2 &&
            resource.document.value(QStringLiteral("case_id")).toString() == case_id) {
            source_reviews.push_back(&resource);
        }
    }
    if (source_reviews.size() != 1) {
        return fail(
            review, QStringLiteral("case_id"),
            QStringLiteral("detached profile requires exactly one same-case source review"));
    }
    const auto& source = *source_reviews.front();
    const auto source_date = source.document.value(QStringLiteral("reviewed_on")).toString();
    const auto detached_date = review.document.value(QStringLiteral("reviewed_on")).toString();
    if (source.document.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independent_review_pending") ||
        !isCanonicalDate(source_date) || !isCanonicalDate(detached_date) ||
        detached_date < source_date) {
        return fail(
            review, QStringLiteral("reviewed_on"),
            QStringLiteral("detached review date precedes or has an invalid pending source"));
    }

    const auto source_traces = source.document.value(QStringLiteral("evidence"))
                                   .toObject()
                                   .value(QStringLiteral("traces"))
                                   .toArray();
    const auto detached_traces = review.document.value(QStringLiteral("evidence"))
                                     .toObject()
                                     .value(QStringLiteral("traces"))
                                     .toArray();
    if (source_traces.isEmpty() || source_traces.size() > maximum_authoring_trace_count ||
        source_traces.size() != detached_traces.size()) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("detached traces differ from the pending source"));
    }
    std::optional<std::pair<QString, QString>> previous_key;
    for (qsizetype index = 0; index < source_traces.size(); ++index) {
        const auto source_trace = source_traces.at(index).toObject();
        const auto key = std::pair{source_trace.value(QStringLiteral("trace_id")).toString(),
                                   source_trace.value(QStringLiteral("evidence_id")).toString()};
        if (source_trace.value(QStringLiteral("engine_revision")).toString() !=
                multiTraceAuthoringEngineRevision() ||
            (previous_key.has_value() && !(previous_key.value() < key))) {
            return fail(review, QStringLiteral("evidence/traces"),
                        QStringLiteral("pending source traces are not canonical production-multi"));
        }
        previous_key = key;
        auto expected = source_trace;
        expected.insert(QStringLiteral("engine_revision"), detachedReviewEngineRevision());
        expected.insert(QStringLiteral("digest"), traceDigest(case_id, expected));
        if (detached_traces.at(index).toObject() != expected) {
            return fail(review, QStringLiteral("evidence/traces"),
                        QStringLiteral("detached traces do not exactly replay the pending source"));
        }
    }

    for (const auto* dimension_name : dimension_names) {
        if (complete_dimension_groups.value(QString::fromLatin1(dimension_name)).size() >
            maximum_dimension_evidence_reference_count) {
            return fail(
                review,
                QStringLiteral("evidence/dimension_evidence/") +
                    QString::fromLatin1(dimension_name),
                QStringLiteral("detached latent evidence partition exceeds 512 references"));
        }
    }
    return validateDetachedHumanFields(review);
}

[[nodiscard]] CapabilityBinding parseCapability(const QJsonObject& object) {
    return CapabilityBinding{
        object.value(QStringLiteral("id")).toString(),
        static_cast<std::uint32_t>(object.value(QStringLiteral("version")).toDouble()),
    };
}

[[nodiscard]] DependencyBinding parseDependency(const QJsonObject& object) {
    return DependencyBinding{object.value(QStringLiteral("pack_id")).toString(),
                             object.value(QStringLiteral("version")).toString()};
}

[[nodiscard]] PackBinding parsePack(const QJsonObject& object) {
    PackBinding binding{
        object.value(QStringLiteral("pack_id")).toString(),
        object.value(QStringLiteral("version")).toString(),
        static_cast<std::uint32_t>(
            object.value(QStringLiteral("manifest_schema_version")).toDouble()),
        {},
        {},
    };
    for (const auto& value : object.value(QStringLiteral("required_capabilities")).toArray()) {
        binding.capabilities.push_back(parseCapability(value.toObject()));
    }
    for (const auto& value : object.value(QStringLiteral("dependencies")).toArray()) {
        binding.dependencies.push_back(parseDependency(value.toObject()));
    }
    std::ranges::sort(binding.capabilities, capabilityLess);
    std::ranges::sort(binding.dependencies, dependencyLess);
    return binding;
}

[[nodiscard]] ResourceBinding parseResource(const QJsonObject& object) {
    return ResourceBinding{
        object.value(QStringLiteral("evidence_id")).toString(),
        object.value(QStringLiteral("owner_pack_id")).toString(),
        object.value(QStringLiteral("owner_pack_version")).toString(),
        object.value(QStringLiteral("resource_id")).toString(),
        object.value(QStringLiteral("resource_kind")).toString(),
        static_cast<std::uint32_t>(object.value(QStringLiteral("schema_version")).toDouble()),
        object.value(QStringLiteral("path")).toString(),
        object.value(QStringLiteral("sha256")).toString(),
    };
}

[[nodiscard]] BlobBinding parseBlob(const QJsonObject& object) {
    return BlobBinding{
        object.value(QStringLiteral("evidence_id")).toString(),
        object.value(QStringLiteral("owner_pack_id")).toString(),
        object.value(QStringLiteral("owner_pack_version")).toString(),
        object.value(QStringLiteral("path")).toString(),
        object.value(QStringLiteral("media_type")).toString(),
        static_cast<std::uint64_t>(object.value(QStringLiteral("byte_size")).toDouble()),
        object.value(QStringLiteral("sha256")).toString(),
    };
}

[[nodiscard]] bool hasCapability(const LoadedPack& pack, std::string_view id,
                                 std::uint32_t version) {
    return std::ranges::any_of(pack.required_capabilities,
                               [id, version](const model::RequiredCapability& capability) {
                                   return capability.id == id && capability.version == version;
                               });
}

[[nodiscard]] bool usesRealismEvidenceShape(const ValidatedResource& review) {
    return review.document.contains(QStringLiteral("evidence")) ||
           review.document.contains(QStringLiteral("reviewer")) ||
           std::ranges::any_of(
               review.document.value(QStringLiteral("known_uncertainty")).toArray(),
               [](const QJsonValue& uncertainty) { return uncertainty.isObject(); });
}

[[nodiscard]] QString recomputedPackRevisionDigest(const LoadedPack& pack) {
    auto capabilities = pack.required_capabilities;
    auto dependencies = pack.dependencies;
    std::vector<const ValidatedResource*> resources;
    resources.reserve(pack.resources.size());
    for (const auto& resource : pack.resources) {
        resources.push_back(&resource);
    }
    auto blobs = pack.blobs;
    std::ranges::sort(capabilities, {}, &model::RequiredCapability::id);
    std::ranges::sort(dependencies, [](const auto& left, const auto& right) {
        return std::tie(left.revision.id.value, left.revision.version, left.revision.digest) <
               std::tie(right.revision.id.value, right.revision.version, right.revision.digest);
    });
    std::ranges::sort(resources, [](const auto* left, const auto* right) {
        return std::tie(left->descriptor.id, left->descriptor.kind, left->descriptor.path) <
               std::tie(right->descriptor.id, right->descriptor.kind, right->descriptor.path);
    });
    std::ranges::sort(blobs, [](const auto& left, const auto& right) {
        return std::tie(left.path, left.media_type, left.byte_size, left.sha256) <
               std::tie(right.path, right.media_type, right.byte_size, right.sha256);
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-pack-revision-v2"));
    addUint64(hash, pack.manifest_schema_version);
    addFrame(hash, QString::fromStdString(pack.revision.id.value));
    addFrame(hash, QString::fromStdString(pack.revision.version));
    addUint64(hash, capabilities.size());
    for (const auto& capability : capabilities) {
        addFrame(hash, QString::fromStdString(capability.id));
        addUint64(hash, capability.version);
    }
    addUint64(hash, dependencies.size());
    for (const auto& dependency : dependencies) {
        addFrame(hash, QString::fromStdString(dependency.revision.id.value));
        addFrame(hash, QString::fromStdString(dependency.revision.version));
        addFrame(hash, QString::fromStdString(dependency.revision.digest));
    }
    addUint64(hash, resources.size());
    for (const auto* resource : resources) {
        addFrame(hash, QString::fromStdString(resource->descriptor.id));
        addFrame(hash, kindName(resource->descriptor.kind));
        addUint64(hash, resource->descriptor.schema_version);
        addFrame(hash, QString::fromStdString(resource->descriptor.path));
        addFrame(hash, QString::fromStdString(resource->descriptor.sha256));
    }
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addFrame(hash, QString::fromStdString(blob.path));
        addFrame(hash, QString::fromStdString(blob.media_type));
        addUint64(hash, blob.byte_size);
        addFrame(hash, QString::fromStdString(blob.sha256));
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QJsonObject pinnedLegacyReviewDocument() {
    return QJsonObject{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
        {QStringLiteral("resource_id"), QStringLiteral("example.review.fictional")},
        {QStringLiteral("case_id"), QStringLiteral("example.case.fictional")},
        {QStringLiteral("review_state"), QStringLiteral("self_reviewed")},
        {QStringLiteral("dimensions"),
         QJsonObject{
             {QStringLiteral("procedural_law"), 2},
             {QStringLiteral("deadlines_authority"), 2},
             {QStringLiteral("record_consistency"), 3},
             {QStringLiteral("consequences"), 2},
             {QStringLiteral("oral_argument"), 3},
             {QStringLiteral("bench_differentiation"), 2},
             {QStringLiteral("provenance"), 2},
         }},
        {QStringLiteral("known_uncertainty"),
         QJsonArray{QStringLiteral("The court and authorities are deliberately fictional.")}},
    };
}

[[nodiscard]] bool isPinnedLegacyV2Review(const ValidatedResource& review, const LoadedPack& pack) {
    if (pack.manifest_schema_version != 2 || pack.revision.id.value != "example.full.fictional" ||
        pack.revision.version != "2.0.0" || review.document != pinnedLegacyReviewDocument() ||
        recomputedPackRevisionDigest(pack).toStdString() != pack.revision.digest) {
        return false;
    }
    constexpr std::array pinned_digests{
        std::string_view{"a9c912ad7e23620f9a5c9f5fb81c9edabe1d00010551c4636e8a621b00655bd4"},
        std::string_view{"bb5e15c14407788a7d9e5370efa610cd12e84a09ca598781bc2f37210f1d4f8d"},
        std::string_view{"e36b712c5f845148a61b65992077119551c0521e39679b0f1572f76217882b54"},
    };
    return std::ranges::find(pinned_digests, pack.revision.digest) != pinned_digests.end();
}

[[nodiscard]] std::expected<void, Error>
validateReview(const ValidatedResource& review, const LoadedPack& review_owner,
               std::span<const LoadedPack* const> all_packs) {
    if (!review.document.contains(QStringLiteral("evidence"))) {
        return fail(
            review, QStringLiteral("evidence"),
            QStringLiteral("typed realism-evidence fields require the complete evidence object"));
    }
    if (!hasCapability(review_owner, "workbench.pack.realism-evidence", 1)) {
        return fail(review, QStringLiteral("evidence"),
                    QStringLiteral("the owning pack does not declare realism-evidence v1"));
    }

    const auto case_id = review.document.value(QStringLiteral("case_id")).toString();
    const auto closure_result = buildSubjectClosure(review, case_id, all_packs);
    if (!closure_result) {
        return std::unexpected(closure_result.error());
    }
    const auto& closure = *closure_result;
    const auto evidence = review.document.value(QStringLiteral("evidence")).toObject();

    std::vector<PackBinding> declared_packs;
    for (const auto& value : evidence.value(QStringLiteral("packs")).toArray()) {
        declared_packs.push_back(parsePack(value.toObject()));
    }
    std::ranges::sort(declared_packs, packLess);
    if (declared_packs != closure.pack_bindings) {
        return fail(
            review, QStringLiteral("evidence/packs"),
            QStringLiteral("pack metadata or dependency topology differs from the subject"));
    }

    QSet<QString> evidence_ids;
    const auto addEvidenceId = [&](const QJsonObject& object) -> bool {
        const auto id = object.value(QStringLiteral("evidence_id")).toString();
        if (evidence_ids.contains(id)) {
            return false;
        }
        evidence_ids.insert(id);
        return true;
    };

    std::vector<ResourceBinding> declared_resources;
    std::vector<ResourceBinding> declared_resource_descriptors;
    QHash<QString, QString> declared_resource_evidence_by_id;
    for (const auto& value : evidence.value(QStringLiteral("resources")).toArray()) {
        const auto object = value.toObject();
        if (!addEvidenceId(object)) {
            return fail(review, QStringLiteral("evidence/resources/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        auto binding = parseResource(object);
        declared_resource_evidence_by_id.insert(binding.resource_id, binding.evidence_id);
        declared_resources.push_back(binding);
        binding.evidence_id.clear();
        declared_resource_descriptors.push_back(std::move(binding));
    }
    std::ranges::sort(declared_resources, resourceLess);
    std::ranges::sort(declared_resource_descriptors, resourceLess);
    if (declared_resource_descriptors != closure.resource_bindings) {
        return fail(review, QStringLiteral("evidence/resources"),
                    QStringLiteral("resource descriptors do not equal the subject closure"));
    }

    std::vector<BlobBinding> declared_blobs;
    std::vector<BlobBinding> declared_blob_descriptors;
    QHash<QString, QString> declared_blob_evidence_by_owner_path;
    for (const auto& value : evidence.value(QStringLiteral("blobs")).toArray()) {
        const auto object = value.toObject();
        if (!addEvidenceId(object)) {
            return fail(review, QStringLiteral("evidence/blobs/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        auto binding = parseBlob(object);
        declared_blob_evidence_by_owner_path.insert(binding.owner_pack_id + u'\n' + binding.path,
                                                    binding.evidence_id);
        declared_blobs.push_back(binding);
        binding.evidence_id.clear();
        declared_blob_descriptors.push_back(std::move(binding));
    }
    std::ranges::sort(declared_blobs, blobLess);
    std::ranges::sort(declared_blob_descriptors, blobLess);
    if (declared_blob_descriptors != closure.blob_bindings) {
        return fail(review, QStringLiteral("evidence/blobs"),
                    QStringLiteral("blob descriptors do not equal the subject closure"));
    }

    const auto computed_closure_digest =
        closureDigest(case_id, closure.pack_bindings, declared_resources, declared_blobs);
    if (evidence.value(QStringLiteral("closure_digest")).toString() != computed_closure_digest) {
        return fail(review, QStringLiteral("evidence/closure_digest"),
                    QStringLiteral("digest does not match the exact review-excluded closure"));
    }

    const auto projected =
        loadRuntimePackForEvidence(*closure.case_owner, closure.subject_packs_dependency_first,
                                   usesCodeOwnedTraceProfile(review.document));
    if (!projected) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("the exact subject cannot be projected for replay: %1")
                        .arg(QString::fromStdString(projected.error().message)));
    }
    const auto runtime_case = std::ranges::find(
        projected->cases, case_id.toStdString(),
        [](const RuntimeCase& candidate) { return candidate.definition.id.value; });
    if (runtime_case == projected->cases.end()) {
        return fail(review, QStringLiteral("case_id"),
                    QStringLiteral("the projected subject case is absent"));
    }
    QSet<QString> trace_ids;
    QSet<QString> authoring_trace_refs;
    qsizetype authoring_trace_count = 0;
    qsizetype multi_trace_authoring_count = 0;
    qsizetype detached_review_trace_count = 0;
    for (const auto& value : evidence.value(QStringLiteral("traces")).toArray()) {
        const auto trace = value.toObject();
        if (!addEvidenceId(trace)) {
            return fail(review, QStringLiteral("evidence/traces/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        const auto trace_id = trace.value(QStringLiteral("trace_id")).toString();
        if (trace_ids.contains(trace_id)) {
            return fail(review, QStringLiteral("evidence/traces/trace_id"),
                        QStringLiteral("trace IDs must be unique"));
        }
        trace_ids.insert(trace_id);
        const auto engine_revision = trace.value(QStringLiteral("engine_revision")).toString();
        if (engine_revision == authoringEngineRevision()) {
            ++authoring_trace_count;
            authoring_trace_refs.insert(trace.value(QStringLiteral("evidence_id")).toString());
        } else if (engine_revision == multiTraceAuthoringEngineRevision()) {
            ++multi_trace_authoring_count;
            authoring_trace_refs.insert(trace.value(QStringLiteral("evidence_id")).toString());
        } else if (engine_revision == detachedReviewEngineRevision()) {
            ++detached_review_trace_count;
            authoring_trace_refs.insert(trace.value(QStringLiteral("evidence_id")).toString());
        }
        const auto normalized = normalizeExecutedTrace(review, case_id, trace, *runtime_case);
        if (!normalized) {
            return std::unexpected(normalized.error());
        }
        if (*normalized != trace) {
            return fail(review, QStringLiteral("evidence/traces"),
                        QStringLiteral("complete reviews must declare every derived trace field"));
        }
    }

    const auto record_id =
        closure.case_resource->document.value(QStringLiteral("record_id")).toString().toStdString();
    const auto record = closure.resources_by_id.find(record_id);
    if (record == closure.resources_by_id.end() ||
        record->second.resource->descriptor.kind != model::ResourceKind::Record) {
        return fail(review, QStringLiteral("evidence/record_checks/record_id"),
                    QStringLiteral("the subject record is absent"));
    }
    const ResourceBinding record_binding{
        {},
        QString::fromStdString(record->second.owner->revision.id.value),
        QString::fromStdString(record->second.owner->revision.version),
        QString::fromStdString(record->second.resource->descriptor.id),
        kindName(record->second.resource->descriptor.kind),
        record->second.resource->descriptor.schema_version,
        QString::fromStdString(record->second.resource->descriptor.path),
        QString::fromStdString(record->second.resource->descriptor.sha256),
    };
    std::vector<BlobBinding> record_blobs;
    QSet<QString> authoring_record_blob_refs;
    for (const auto& entry_value :
         record->second.resource->document.value(QStringLiteral("docket_entries")).toArray()) {
        const auto path = entry_value.toObject().value(QStringLiteral("asset_path")).toString();
        const auto found = closure.blobs_by_owner_path.find(
            record->second.owner->revision.id.value + '\n' + path.toStdString());
        if (found == closure.blobs_by_owner_path.end()) {
            return fail(review, QStringLiteral("evidence/record_checks"),
                        QStringLiteral("a subject record blob is absent"));
        }
        record_blobs.push_back(found->second);
        const auto evidence_ref = declared_blob_evidence_by_owner_path.value(
            QString::fromStdString(record->second.owner->revision.id.value) + u'\n' + path);
        if (!evidence_ref.isEmpty()) {
            authoring_record_blob_refs.insert(evidence_ref);
        }
    }
    QSet<QString> record_check_ids;
    QSet<QString> authoring_record_check_refs;
    QSet<QString> authoring_record_check_kinds;
    QHash<QString, QString> authoring_record_check_ref_by_kind;
    QHash<QString, QString> authoring_record_check_id_by_kind;
    for (const auto& value : evidence.value(QStringLiteral("record_checks")).toArray()) {
        const auto check = value.toObject();
        if (!addEvidenceId(check)) {
            return fail(review, QStringLiteral("evidence/record_checks/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        const auto check_id = check.value(QStringLiteral("check_id")).toString();
        if (record_check_ids.contains(check_id)) {
            return fail(review, QStringLiteral("evidence/record_checks/check_id"),
                        QStringLiteral("record-check IDs must be unique"));
        }
        record_check_ids.insert(check_id);
        auto check_blobs = std::vector<BlobBinding>{};
        const auto check_kind = check.value(QStringLiteral("check_kind")).toString();
        authoring_record_check_refs.insert(check.value(QStringLiteral("evidence_id")).toString());
        authoring_record_check_kinds.insert(check_kind);
        authoring_record_check_ref_by_kind.insert(
            check_kind, check.value(QStringLiteral("evidence_id")).toString());
        authoring_record_check_id_by_kind.insert(check_kind, check_id);
        if (check_kind == QStringLiteral("asset_resolution")) {
            check_blobs = record_blobs;
        } else if (check_kind != QStringLiteral("page_anchor_resolution")) {
            return fail(review, QStringLiteral("evidence/record_checks/check_kind"),
                        QStringLiteral("record-check kind is unsupported"));
        }
        if (check.value(QStringLiteral("record_id")).toString().toStdString() != record_id ||
            check.value(QStringLiteral("digest")).toString() !=
                recordCheckDigest(case_id, check, record_binding, std::move(check_blobs))) {
            return fail(review, QStringLiteral("evidence/record_checks"),
                        QStringLiteral("record check subject or digest is stale"));
        }
    }

    QSet<QString> declared_authority_ids;
    QHash<QString, QString> declared_authority_evidence_by_id;
    for (const auto& value : evidence.value(QStringLiteral("authorities")).toArray()) {
        const auto authority = value.toObject();
        if (!addEvidenceId(authority)) {
            return fail(review, QStringLiteral("evidence/authorities/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        const auto authority_id = authority.value(QStringLiteral("authority_id")).toString();
        if (declared_authority_ids.contains(authority_id)) {
            return fail(review, QStringLiteral("evidence/authorities/authority_id"),
                        QStringLiteral("authority evidence bindings must be unique"));
        }
        declared_authority_ids.insert(authority_id);
        declared_authority_evidence_by_id.insert(
            authority_id, authority.value(QStringLiteral("evidence_id")).toString());
        if (!closure.authority_ids.contains(authority_id)) {
            return fail(review, QStringLiteral("evidence/authorities/authority_id"),
                        QStringLiteral("authority is outside the subject closure"));
        }
    }

    bool has_score_three = false;
    const auto dimensions = review.document.value(QStringLiteral("dimensions")).toObject();
    const auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    for (const auto* dimension_name : dimension_names) {
        const auto key = QLatin1StringView(dimension_name);
        const auto score = static_cast<int>(dimensions.value(key).toDouble());
        const auto references = dimension_evidence.value(key).toArray();
        has_score_three = has_score_three || score == 3;
        if ((score == 0) != references.isEmpty() ||
            !std::ranges::all_of(references, [&evidence_ids](const QJsonValue& reference) {
                return evidence_ids.contains(reference.toString());
            })) {
            return fail(
                review, QStringLiteral("evidence/dimension_evidence/") + key,
                QStringLiteral(
                    "score evidence must be nonempty exactly for nonzero scores and resolve"));
        }
    }

    const auto declared_trace_count = evidence.value(QStringLiteral("traces")).toArray().size();
    if (authoring_trace_count != 0 && (authoring_trace_count != 1 || declared_trace_count != 1)) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("authoring-profile evidence requires exactly one trace"));
    }
    if (multi_trace_authoring_count != 0 && multi_trace_authoring_count != declared_trace_count) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("multi-trace authoring-profile evidence requires every trace "
                                   "to use its exact engine revision"));
    }
    if (detached_review_trace_count != 0 && detached_review_trace_count != declared_trace_count) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("detached-review evidence requires every trace to use its exact "
                                   "engine revision"));
    }

    const auto uses_single_trace_profile = authoring_trace_count != 0;
    const auto uses_multi_trace_profile = multi_trace_authoring_count != 0;
    const auto uses_detached_review_profile = detached_review_trace_count != 0;
    if (uses_single_trace_profile || uses_multi_trace_profile || uses_detached_review_profile) {
        const auto maximum_score = uses_single_trace_profile ? 1 : uses_multi_trace_profile ? 2 : 3;
        for (const auto* dimension_name : dimension_names) {
            const auto key = QLatin1StringView(dimension_name);
            if (static_cast<int>(dimensions.value(key).toDouble()) > maximum_score) {
                return fail(
                    review, QStringLiteral("dimensions/") + key,
                    uses_single_trace_profile
                        ? QStringLiteral(
                              "authoring-profile evidence can substantiate level 1 at most")
                        : QStringLiteral(
                              "multi-trace authoring-profile evidence can substantiate level 2 "
                              "at most"));
            }
        }

        const auto profile_result = buildAuthoringEvidenceProfile(closure, case_id);
        if (!profile_result) {
            return fail(review, QStringLiteral("evidence/authorities"), profile_result.error());
        }
        const auto& profile = *profile_result;
        const auto prerequisites = validateCodeOwnedScorePrerequisites(review, profile);
        if (!prerequisites) {
            return prerequisites;
        }
        for (const auto& binding : closure.resource_bindings) {
            const auto expected =
                evidenceId(QStringLiteral("resource"),
                           {binding.owner_pack_id, binding.owner_pack_version, binding.resource_id,
                            binding.resource_kind, QString::number(binding.schema_version),
                            binding.path, binding.sha256});
            if (declared_resource_evidence_by_id.value(binding.resource_id) != expected) {
                return fail(review, QStringLiteral("evidence/resources/evidence_id"),
                            QStringLiteral("authoring-profile resource evidence ID is stale"));
            }
        }
        for (const auto& binding : closure.blob_bindings) {
            const auto expected = evidenceId(QStringLiteral("blob"),
                                             {binding.owner_pack_id, binding.owner_pack_version,
                                              binding.path, binding.media_type,
                                              QString::number(binding.byte_size), binding.sha256});
            if (declared_blob_evidence_by_owner_path.value(binding.owner_pack_id + u'\n' +
                                                           binding.path) != expected) {
                return fail(review, QStringLiteral("evidence/blobs/evidence_id"),
                            QStringLiteral("authoring-profile blob evidence ID is stale"));
            }
        }
        if (declared_authority_ids != profile.all_authority_ids) {
            return fail(
                review, QStringLiteral("evidence/authorities"),
                QStringLiteral("authoring-profile authorities differ from exact relevant scope"));
        }
        for (const auto& authority_id : profile.all_authority_ids) {
            if (declared_authority_evidence_by_id.value(authority_id) !=
                evidenceId(QStringLiteral("authority"), {case_id, authority_id})) {
                return fail(review, QStringLiteral("evidence/authorities/evidence_id"),
                            QStringLiteral("authoring-profile authority binding ID is stale"));
            }
        }

        const QSet<QString> required_check_kinds{QStringLiteral("asset_resolution"),
                                                 QStringLiteral("page_anchor_resolution")};
        if (authoring_record_check_kinds != required_check_kinds ||
            evidence.value(QStringLiteral("record_checks")).toArray().size() != 2) {
            return fail(
                review, QStringLiteral("evidence/record_checks"),
                QStringLiteral("authoring-profile evidence requires both exact record checks"));
        }
        for (const auto& check_kind : required_check_kinds) {
            if (authoring_record_check_ref_by_kind.value(check_kind) !=
                evidenceId(QStringLiteral("record-check"), {case_id, check_kind})) {
                return fail(review, QStringLiteral("evidence/record_checks/evidence_id"),
                            QStringLiteral("authoring-profile record-check ID is stale"));
            }
            if (authoring_record_check_id_by_kind.value(check_kind) !=
                recordCheckId(check_kind, case_id)) {
                return fail(review, QStringLiteral("evidence/record_checks/check_id"),
                            QStringLiteral("authoring-profile record check_id is stale"));
            }
        }

        const AuthoringEvidenceBindings bindings{
            declared_resource_evidence_by_id,
            declared_authority_evidence_by_id,
            authoring_record_blob_refs,
            authoring_record_check_refs,
            authoring_trace_refs,
        };
        const auto expected_groups = authoringDimensionGroups(profile, bindings);
        for (const auto* dimension_name : dimension_names) {
            const auto key = QString::fromLatin1(dimension_name);
            const auto score = static_cast<int>(dimensions.value(key).toDouble());
            auto expected = expected_groups.value(key);
            if (score == 0) {
                expected.clear();
            } else if (expected.isEmpty()) {
                return fail(
                    review, QStringLiteral("evidence/dimension_evidence/") + key,
                    QStringLiteral("authoring-profile dimension has no relevant exact evidence"));
            }
            const auto declared_values = dimension_evidence.value(key).toArray();
            QSet<QString> declared;
            for (const auto& value : declared_values) {
                declared.insert(value.toString());
            }
            if (declared.size() != declared_values.size() || declared != expected) {
                return fail(
                    review, QStringLiteral("evidence/dimension_evidence/") + key,
                    QStringLiteral("authoring-profile dimension references differ from the exact "
                                   "relevant partition"));
            }
        }
        if (uses_detached_review_profile) {
            const auto detached =
                validateDetachedProfile(review, review_owner, closure, expected_groups);
            if (!detached) {
                return detached;
            }
        }
    }

    QSet<QString> uncertainty_ids;
    for (const auto& value : review.document.value(QStringLiteral("known_uncertainty")).toArray()) {
        if (!value.isObject()) {
            return fail(review, QStringLiteral("known_uncertainty"),
                        QStringLiteral("evidence-bearing reviews require typed uncertainties"));
        }
        const auto uncertainty = value.toObject();
        const auto uncertainty_id = uncertainty.value(QStringLiteral("uncertainty_id")).toString();
        if (uncertainty_ids.contains(uncertainty_id) ||
            (uncertainty.value(QStringLiteral("blocking")).toBool() &&
             !uncertainty.contains(QStringLiteral("remediation_issue")))) {
            return fail(review, QStringLiteral("known_uncertainty"),
                        QStringLiteral(
                            "uncertainties must be unique and blocking items require remediation"));
        }
        uncertainty_ids.insert(uncertainty_id);
    }

    const auto review_state = review.document.value(QStringLiteral("review_state")).toString();
    const auto independently_reviewed = review_state == QStringLiteral("independently_reviewed");
    if (has_score_three && !independently_reviewed) {
        return fail(review, QStringLiteral("dimensions"),
                    QStringLiteral("self or pending review cannot claim realism level 3"));
    }
    if (!independently_reviewed) {
        if (&review_owner != closure.case_owner) {
            return fail(review, QStringLiteral("case_id"),
                        QStringLiteral("self or pending evidence must be owned with its case"));
        }
        return {};
    }

    const auto direct_exact_subject =
        std::ranges::any_of(review_owner.dependencies, [&closure](const auto& dependency) {
            return dependency.revision == closure.case_owner->revision;
        });
    const auto detached_resources =
        std::ranges::all_of(review_owner.resources, [](const ValidatedResource& resource) {
            return resource.descriptor.kind == model::ResourceKind::RealismReview;
        });
    if (&review_owner == closure.case_owner || !direct_exact_subject || !detached_resources ||
        !review_owner.blobs.empty() || !review.document.contains(QStringLiteral("reviewed_on")) ||
        !review.document.contains(QStringLiteral("reviewer_reference")) ||
        !review.document.contains(QStringLiteral("reviewer"))) {
        return fail(review, QStringLiteral("review_state"),
                    QStringLiteral("independent evidence requires a metadata-complete detached "
                                   "review pack with a direct exact subject pin"));
    }
    return {};
}

} // namespace

std::expected<void, Error>
validateRealismEvidence(const LoadedPack& root,
                        std::span<const LoadedPack* const> dependencies_dependency_first) {
    std::vector<const LoadedPack*> all_packs;
    all_packs.reserve(dependencies_dependency_first.size() + 1U);
    for (const auto* dependency : dependencies_dependency_first) {
        if (dependency == nullptr) {
            return std::unexpected(Error{ErrorCode::CrossReferenceFailure,
                                         QStringLiteral("Realism evidence closure is incomplete")});
        }
        all_packs.push_back(dependency);
    }
    all_packs.push_back(&root);

    for (const auto* pack : all_packs) {
        const auto owns_realism_review =
            std::ranges::any_of(pack->resources, [](const ValidatedResource& resource) {
                return resource.descriptor.kind == model::ResourceKind::RealismReview;
            });
        if (hasCapability(*pack, "workbench.pack.realism-evidence", 1) && !owns_realism_review) {
            return std::unexpected(Error{
                ErrorCode::CrossReferenceFailure,
                QStringLiteral("Pack %1 declares realism-evidence v1 without a realism review")
                    .arg(QString::fromStdString(pack->revision.id.value)),
            });
        }
        for (const auto& resource : pack->resources) {
            if (!usesCodeOwnedTraceProfile(resource.document)) {
                continue;
            }
            if (resource.descriptor.kind != model::ResourceKind::RealismReview ||
                resource.descriptor.schema_version != 2) {
                return fail(resource, QStringLiteral("descriptor"),
                            QStringLiteral("code-owned realism profiles require a schema-2 "
                                           "realism-review descriptor"));
            }
            if (!hasOnlyUnicodeScalars(resource.document)) {
                return fail(resource, QStringLiteral("document"),
                            QStringLiteral("code-owned realism profiles require Unicode-scalar "
                                           "object keys and string values"));
            }
        }
    }

    for (const auto* pack : all_packs) {
        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind != model::ResourceKind::RealismReview ||
                resource.descriptor.schema_version == 1 ||
                (!usesRealismEvidenceShape(resource) && isPinnedLegacyV2Review(resource, *pack))) {
                continue;
            }
            const auto validated = validateReview(resource, *pack, all_packs);
            if (!validated) {
                return std::unexpected(validated.error());
            }
        }
    }
    return {};
}

std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>
authorRealismEvidence(const PackCatalog& catalog, const RealismEvidenceAuthoringInput& input) {
    if (input.root_directory.trimmed().isEmpty() || input.review_resource_id.trimmed().isEmpty() ||
        input.trace.isEmpty()) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidInput,
            QStringLiteral("Root directory, review resource ID, and trace are required"));
    }
    return authorRealismEvidence(catalog, RealismEvidenceTraceSetAuthoringInput{
                                              input.root_directory,
                                              input.review_resource_id,
                                              QJsonArray{input.trace},
                                              RealismEvidenceTraceSetProfile::SingleTraceHelperV1,
                                          });
}

std::expected<AuthoredRealismEvidence, RealismEvidenceAuthoringError>
authorRealismEvidence(const PackCatalog& catalog,
                      const RealismEvidenceTraceSetAuthoringInput& input) {
    if (input.profile != RealismEvidenceTraceSetProfile::SingleTraceHelperV1 &&
        input.profile != RealismEvidenceTraceSetProfile::MultiTraceProductionV1) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                QStringLiteral("Unknown realism-evidence authoring profile"));
    }
    const auto single_trace_profile =
        input.profile == RealismEvidenceTraceSetProfile::SingleTraceHelperV1;
    if (input.root_directory.trimmed().isEmpty() || input.review_resource_id.trimmed().isEmpty() ||
        input.traces.isEmpty()) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidInput,
            QStringLiteral("Root directory, review resource ID, and traces are required"));
    }
    if (input.traces.size() > maximum_authoring_trace_count) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                QStringLiteral("Trace sets may contain at most 256 traces"));
    }
    if (single_trace_profile && input.traces.size() != 1) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidInput,
            QStringLiteral("The single-trace helper profile requires exactly one trace"));
    }

    const auto loaded = PackReader::readDirectoryForRealismAuthoring(input.root_directory,
                                                                     input.review_resource_id);
    if (!loaded) {
        return invalidPack(loaded.error());
    }
    if (loaded->manifest_schema_version != 2) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("Realism evidence authoring requires a schema-v2 root pack"));
    }
    if (!hasCapability(*loaded, "workbench.pack.realism-evidence", 1)) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The root pack must declare workbench.pack.realism-evidence v1"));
    }

    const ValidatedResource* review = nullptr;
    for (const auto& resource : loaded->resources) {
        if (QString::fromStdString(resource.descriptor.id) != input.review_resource_id) {
            continue;
        }
        if (review != nullptr || resource.descriptor.kind != model::ResourceKind::RealismReview ||
            resource.descriptor.schema_version != 2) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                    QStringLiteral("The requested ID must identify exactly one "
                                                   "root-owned schema-v2 realism review"));
        }
        review = &resource;
    }
    if (review == nullptr) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The requested root-owned schema-v2 realism review is absent"));
    }

    const auto review_state = review->document.value(QStringLiteral("review_state")).toString();
    if (review_state == QStringLiteral("independently_reviewed")) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The authoring command cannot claim or rewrite independent review"));
    }
    const auto dimensions = review->document.value(QStringLiteral("dimensions")).toObject();
    const auto maximum_score = single_trace_profile ? 1 : 2;
    for (const auto* dimension_name : dimension_names) {
        if (static_cast<int>(dimensions.value(QLatin1StringView(dimension_name)).toDouble()) >
            maximum_score) {
            return authoringFailure(
                RealismEvidenceAuthoringErrorCode::InvalidPack,
                single_trace_profile
                    ? QStringLiteral(
                          "The authoring command can substantiate realism level 1 at most")
                    : QStringLiteral(
                          "The multi-trace authoring command can substantiate realism level 2 "
                          "at most"));
        }
    }

    static const QSet<QString> allowed_trace_fields{
        QStringLiteral("evidence_id"),   QStringLiteral("trace_id"),
        QStringLiteral("workflow_id"),   QStringLiteral("engine_revision"),
        QStringLiteral("journal"),       QStringLiteral("command_count"),
        QStringLiteral("event_count"),   QStringLiteral("journal_sha256"),
        QStringLiteral("operation_ids"), QStringLiteral("terminal_stage_id"),
        QStringLiteral("digest"),
    };
    const auto fixed_engine_revision =
        single_trace_profile ? authoringEngineRevision() : multiTraceAuthoringEngineRevision();
    std::vector<QJsonObject> authoring_traces;
    authoring_traces.reserve(static_cast<std::size_t>(input.traces.size()));
    QSet<QString> evidence_ids;
    QSet<QString> trace_ids;
    for (const auto& trace_value : input.traces) {
        if (!trace_value.isObject()) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                    QStringLiteral("Every trace must be a JSON object"));
        }
        auto authoring_trace = trace_value.toObject();
        for (auto iterator = authoring_trace.constBegin(); iterator != authoring_trace.constEnd();
             ++iterator) {
            if (!allowed_trace_fields.contains(iterator.key())) {
                return authoringFailure(
                    RealismEvidenceAuthoringErrorCode::InvalidInput,
                    QStringLiteral("Trace contains unsupported field %1").arg(iterator.key()));
            }
        }
        for (const auto& field : {QStringLiteral("evidence_id"), QStringLiteral("trace_id"),
                                  QStringLiteral("workflow_id")}) {
            if (!authoring_trace.value(field).isString() ||
                authoring_trace.value(field).toString().isEmpty()) {
                return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                        QStringLiteral("Trace field %1 is required").arg(field));
            }
        }
        const auto evidence_id = authoring_trace.value(QStringLiteral("evidence_id")).toString();
        if (evidence_ids.contains(evidence_id)) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                    QStringLiteral("Trace evidence_id values must be unique"));
        }
        evidence_ids.insert(evidence_id);
        const auto trace_id = authoring_trace.value(QStringLiteral("trace_id")).toString();
        if (trace_ids.contains(trace_id)) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                    QStringLiteral("Trace trace_id values must be unique"));
        }
        trace_ids.insert(trace_id);
        if (authoring_trace.contains(QStringLiteral("engine_revision")) &&
            (!authoring_trace.value(QStringLiteral("engine_revision")).isString() ||
             authoring_trace.value(QStringLiteral("engine_revision")).toString() !=
                 fixed_engine_revision)) {
            return authoringFailure(
                RealismEvidenceAuthoringErrorCode::InvalidInput,
                single_trace_profile
                    ? QStringLiteral(
                          "Trace engine_revision must equal the authoring engine revision")
                    : QStringLiteral(
                          "Trace engine_revision must equal the selected authoring profile"));
        }
        authoring_trace.insert(QStringLiteral("engine_revision"), fixed_engine_revision);
        if (!authoring_trace.value(QStringLiteral("journal")).isArray() ||
            authoring_trace.value(QStringLiteral("journal")).toArray().isEmpty()) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidInput,
                                    QStringLiteral("Trace journal must be a nonempty array"));
        }
        authoring_traces.push_back(std::move(authoring_trace));
    }

    const auto review_path = QString::fromStdString(review->descriptor.path);
    const auto manifest_path = QDir(input.root_directory).filePath(QStringLiteral("manifest.json"));
    const auto absolute_review_path = QDir(input.root_directory).filePath(review_path);
    const auto source_review_bytes =
        readAuthoringRegularFile(absolute_review_path, maximum_authoring_review_bytes);
    const auto source_manifest_bytes =
        readAuthoringRegularFile(manifest_path, maximum_authoring_manifest_bytes);
    if (!source_review_bytes || !source_manifest_bytes ||
        sha256(QByteArrayView(*source_review_bytes)).toStdString() != review->descriptor.sha256) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            !source_review_bytes ? source_review_bytes.error()
            : !source_manifest_bytes
                ? source_manifest_bytes.error()
                : QStringLiteral("The authoring source changed while it was being read"));
    }
    const auto confirmed = PackReader::readDirectoryForRealismAuthoring(input.root_directory,
                                                                        input.review_resource_id);
    if (!confirmed) {
        return invalidPack(confirmed.error());
    }
    const ValidatedResource* confirmed_review = nullptr;
    for (const auto& resource : confirmed->resources) {
        if (QString::fromStdString(resource.descriptor.id) == input.review_resource_id) {
            confirmed_review = &resource;
            break;
        }
    }
    const auto current_review_bytes =
        readAuthoringRegularFile(absolute_review_path, maximum_authoring_review_bytes);
    const auto current_manifest_bytes =
        readAuthoringRegularFile(manifest_path, maximum_authoring_manifest_bytes);
    if (confirmed_review == nullptr || confirmed->revision != loaded->revision ||
        confirmed_review->document != review->document ||
        confirmed_review->descriptor.sha256 != review->descriptor.sha256 || !current_review_bytes ||
        !current_manifest_bytes || *current_review_bytes != *source_review_bytes ||
        *current_manifest_bytes != *source_manifest_bytes) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The authoring source changed while it was being read"));
    }
    review = confirmed_review;

    auto baseline_root = *confirmed;
    std::erase_if(baseline_root.resources, [&](const ValidatedResource& resource) {
        return QString::fromStdString(resource.descriptor.id) == input.review_resource_id;
    });
    std::erase_if(baseline_root.required_capabilities, [](const auto& capability) {
        return capability.id == "workbench.pack.realism-evidence";
    });
    const auto baseline = catalog.resolveClosure(baseline_root.revision, &baseline_root);
    if (!baseline) {
        return catalogFailure(baseline.error());
    }

    std::vector<const LoadedPack*> all_packs;
    all_packs.reserve(baseline->dependenciesDependencyFirst().size() + 1U);
    for (const auto& dependency : baseline->dependenciesDependencyFirst()) {
        all_packs.push_back(&dependency);
    }
    all_packs.push_back(&*confirmed);

    const auto case_id = review->document.value(QStringLiteral("case_id")).toString();
    const auto closure_result = buildSubjectClosure(*review, case_id, all_packs);
    if (!closure_result) {
        return invalidPack(closure_result.error());
    }
    auto closure = *closure_result;
    if (closure.case_owner != &*confirmed) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("Self or pending realism evidence must be owned with its exact case"));
    }

    QHash<QString, QString> resource_evidence_by_id;
    QJsonArray resource_values;
    for (auto& binding : closure.resource_bindings) {
        binding.evidence_id =
            evidenceId(QStringLiteral("resource"),
                       {binding.owner_pack_id, binding.owner_pack_version, binding.resource_id,
                        binding.resource_kind, QString::number(binding.schema_version),
                        binding.path, binding.sha256});
        resource_evidence_by_id.insert(binding.resource_id, binding.evidence_id);
        resource_values.push_back(resourceBindingObject(binding));
    }
    QHash<QString, QString> blob_evidence_by_owner_path;
    QJsonArray blob_values;
    for (auto& binding : closure.blob_bindings) {
        binding.evidence_id =
            evidenceId(QStringLiteral("blob"),
                       {binding.owner_pack_id, binding.owner_pack_version, binding.path,
                        binding.media_type, QString::number(binding.byte_size), binding.sha256});
        blob_evidence_by_owner_path.insert(binding.owner_pack_id + u'\n' + binding.path,
                                           binding.evidence_id);
        blob_values.push_back(blobBindingObject(binding));
    }
    QJsonArray pack_values;
    for (const auto& binding : closure.pack_bindings) {
        pack_values.push_back(packBindingObject(binding));
    }

    const auto projected = loadRuntimePackForEvidence(*closure.case_owner,
                                                      closure.subject_packs_dependency_first, true);
    if (!projected) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The exact subject cannot be projected for trace replay: %1")
                .arg(QString::fromStdString(projected.error().message)));
    }
    const auto runtime_case = std::ranges::find(
        projected->cases, case_id.toStdString(),
        [](const RuntimeCase& candidate) { return candidate.definition.id.value; });
    if (runtime_case == projected->cases.end()) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                QStringLiteral("The exact reviewed case is absent at runtime"));
    }
    std::vector<QJsonObject> normalized_trace_objects;
    normalized_trace_objects.reserve(authoring_traces.size());
    for (const auto& authoring_trace : authoring_traces) {
        const auto normalized_trace =
            normalizeExecutedTrace(*review, case_id, authoring_trace, *runtime_case);
        if (!normalized_trace) {
            return invalidPack(normalized_trace.error());
        }
        normalized_trace_objects.push_back(*normalized_trace);
    }
    std::ranges::sort(normalized_trace_objects,
                      [](const QJsonObject& left, const QJsonObject& right) {
                          return std::tuple{left.value(QStringLiteral("trace_id")).toString(),
                                            left.value(QStringLiteral("evidence_id")).toString()} <
                                 std::tuple{right.value(QStringLiteral("trace_id")).toString(),
                                            right.value(QStringLiteral("evidence_id")).toString()};
                      });
    QJsonArray normalized_traces;
    QSet<QString> normalized_trace_refs;
    for (const auto& normalized_trace : normalized_trace_objects) {
        normalized_traces.push_back(normalized_trace);
        normalized_trace_refs.insert(
            normalized_trace.value(QStringLiteral("evidence_id")).toString());
    }

    const auto record_id =
        closure.case_resource->document.value(QStringLiteral("record_id")).toString();
    const auto record_entry = closure.resources_by_id.find(record_id.toStdString());
    if (record_entry == closure.resources_by_id.end() ||
        record_entry->second.resource->descriptor.kind != model::ResourceKind::Record) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                QStringLiteral("The reviewed case record is absent"));
    }
    const auto record_resource = record_entry->second.resource;
    const auto record_owner = record_entry->second.owner;
    const ResourceBinding record_binding{
        {},
        QString::fromStdString(record_owner->revision.id.value),
        QString::fromStdString(record_owner->revision.version),
        QString::fromStdString(record_resource->descriptor.id),
        kindName(record_resource->descriptor.kind),
        record_resource->descriptor.schema_version,
        QString::fromStdString(record_resource->descriptor.path),
        QString::fromStdString(record_resource->descriptor.sha256),
    };
    std::vector<BlobBinding> record_blobs;
    QSet<QString> record_blob_evidence;
    for (const auto& value :
         record_resource->document.value(QStringLiteral("docket_entries")).toArray()) {
        const auto path = value.toObject().value(QStringLiteral("asset_path")).toString();
        const auto owner_path = record_owner->revision.id.value + '\n' + path.toStdString();
        const auto found = closure.blobs_by_owner_path.find(owner_path);
        if (found == closure.blobs_by_owner_path.end()) {
            return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                    QStringLiteral("A reviewed record asset is absent"));
        }
        record_blobs.push_back(found->second);
        const auto evidence = blob_evidence_by_owner_path.value(
            QString::fromStdString(record_owner->revision.id.value) + u'\n' + path);
        if (!evidence.isEmpty()) {
            record_blob_evidence.insert(evidence);
        }
    }

    const auto makeRecordCheck = [&](const QString& kind, std::vector<BlobBinding> blobs) {
        QJsonObject check{
            {QStringLiteral("evidence_id"),
             evidenceId(QStringLiteral("record-check"), {case_id, kind})},
            {QStringLiteral("check_id"), recordCheckId(kind, case_id)},
            {QStringLiteral("record_id"), record_id},
            {QStringLiteral("check_kind"), kind},
        };
        check.insert(QStringLiteral("digest"),
                     recordCheckDigest(case_id, check, record_binding, std::move(blobs)));
        return check;
    };
    const auto asset_check = makeRecordCheck(QStringLiteral("asset_resolution"), record_blobs);
    const auto anchor_check = makeRecordCheck(QStringLiteral("page_anchor_resolution"), {});
    const QJsonArray record_checks{asset_check, anchor_check};

    const auto profile_result = buildAuthoringEvidenceProfile(closure, case_id);
    if (!profile_result) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                profile_result.error());
    }
    const auto& profile = *profile_result;

    QJsonArray authority_values;
    QHash<QString, QString> authority_evidence_by_id;
    auto sorted_authorities = profile.all_authority_ids.values();
    std::ranges::sort(sorted_authorities);
    for (const auto& authority_id : sorted_authorities) {
        const auto binding_id = evidenceId(QStringLiteral("authority"), {case_id, authority_id});
        authority_evidence_by_id.insert(authority_id, binding_id);
        authority_values.push_back(QJsonObject{
            {QStringLiteral("evidence_id"), binding_id},
            {QStringLiteral("authority_id"), authority_id},
        });
    }
    const AuthoringEvidenceBindings authoring_bindings{
        resource_evidence_by_id,
        authority_evidence_by_id,
        record_blob_evidence,
        QSet<QString>{asset_check.value(QStringLiteral("evidence_id")).toString(),
                      anchor_check.value(QStringLiteral("evidence_id")).toString()},
        normalized_trace_refs,
    };
    const auto dimension_groups = authoringDimensionGroups(profile, authoring_bindings);

    QJsonObject dimension_evidence;
    for (const auto* dimension_name : dimension_names) {
        const auto key = QString::fromLatin1(dimension_name);
        const auto score = static_cast<int>(dimensions.value(key).toDouble());
        auto references = dimension_groups.value(key);
        if (score == 0) {
            references.clear();
        } else if (references.isEmpty() ||
                   references.size() > maximum_dimension_evidence_reference_count ||
                   (key == QStringLiteral("oral_argument") &&
                    profile.argument_resource_ids.isEmpty()) ||
                   (key == QStringLiteral("bench_differentiation") &&
                    (profile.bench_resource_ids.isEmpty() ||
                     profile.judge_resource_ids.isEmpty()))) {
            return authoringFailure(
                RealismEvidenceAuthoringErrorCode::InvalidPack,
                QStringLiteral("The selected case cannot substantiate nonzero %1 evidence")
                    .arg(key));
        }
        dimension_evidence.insert(key, sortedReferences(references));
    }

    QJsonObject evidence{
        {QStringLiteral("packs"), pack_values},
        {QStringLiteral("resources"), resource_values},
        {QStringLiteral("blobs"), blob_values},
        {QStringLiteral("traces"), normalized_traces},
        {QStringLiteral("record_checks"), record_checks},
        {QStringLiteral("authorities"), authority_values},
        {QStringLiteral("dimension_evidence"), dimension_evidence},
    };
    const auto closure_digest = closureDigest(case_id, closure.pack_bindings,
                                              closure.resource_bindings, closure.blob_bindings);
    evidence.insert(QStringLiteral("closure_digest"), closure_digest);

    auto authored_review_document = review->document;
    authored_review_document.insert(QStringLiteral("evidence"), evidence);
    const auto schema_validator = SchemaValidator::fromBundledSchemas(2);
    if (!schema_validator) {
        return invalidPack(schema_validator.error());
    }
    if (const auto validated = schema_validator->validate(
            QStringLiteral("realism-review.schema.json"), authored_review_document);
        !validated) {
        return invalidPack(validated.error());
    }
    const auto review_bytes = serializedObject(authored_review_document);
    const auto review_sha256 = sha256(QByteArrayView(review_bytes));

    QJsonParseError manifest_parse_error;
    const auto manifest_document =
        QJsonDocument::fromJson(*source_manifest_bytes, &manifest_parse_error);
    if (manifest_parse_error.error != QJsonParseError::NoError || !manifest_document.isObject()) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                QStringLiteral("Cannot parse the root manifest for authoring"));
    }
    auto manifest = manifest_document.object();
    auto contents = manifest.value(QStringLiteral("contents")).toArray();
    qsizetype descriptor_matches = 0;
    for (qsizetype index = 0; index < contents.size(); ++index) {
        auto descriptor = contents.at(index).toObject();
        if (descriptor.value(QStringLiteral("id")).toString() != input.review_resource_id) {
            continue;
        }
        ++descriptor_matches;
        descriptor.insert(QStringLiteral("sha256"), review_sha256);
        contents.replace(index, descriptor);
    }
    if (descriptor_matches != 1) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::InvalidPack,
            QStringLiteral("The target review manifest descriptor is not unique"));
    }
    manifest.insert(QStringLiteral("contents"), contents);
    if (const auto validated =
            schema_validator->validate(QStringLiteral("manifest.schema.json"), manifest);
        !validated) {
        return invalidPack(validated.error());
    }
    const auto manifest_bytes = serializedObject(manifest);

    auto authored_root = *confirmed;
    qsizetype authored_matches = 0;
    for (auto& resource : authored_root.resources) {
        if (QString::fromStdString(resource.descriptor.id) != input.review_resource_id) {
            continue;
        }
        ++authored_matches;
        resource.document = authored_review_document;
        resource.descriptor.sha256 = review_sha256.toStdString();
    }
    if (authored_matches != 1) {
        return authoringFailure(RealismEvidenceAuthoringErrorCode::InvalidPack,
                                QStringLiteral("The target review is not unique after authoring"));
    }
    authored_root.revision.digest = recomputedPackRevisionDigest(authored_root).toStdString();
    const auto final_resolved = catalog.resolveClosure(authored_root.revision, &authored_root);
    if (!final_resolved) {
        return catalogFailure(final_resolved.error());
    }
    const auto installed_root =
        catalog.load(authored_root.revision.id, authored_root.revision.version);
    if (installed_root && installed_root->revision.digest != authored_root.revision.digest) {
        return authoringFailure(
            RealismEvidenceAuthoringErrorCode::ImmutableConflict,
            QStringLiteral("Catalog already contains a different immutable revision for %1 %2")
                .arg(QString::fromStdString(authored_root.revision.id.value),
                     QString::fromStdString(authored_root.revision.version)));
    }
    if (!installed_root && installed_root.error().code != CatalogErrorCode::NotFound) {
        return catalogFailure(installed_root.error());
    }

    return AuthoredRealismEvidence{
        final_resolved->root().revision,
        case_id,
        input.review_resource_id,
        review_path,
        *source_review_bytes,
        *source_manifest_bytes,
        review_bytes,
        review_sha256,
        manifest_bytes,
        closure_digest,
        RealismEvidenceCounts{
            closure.pack_bindings.size(),
            closure.resource_bindings.size(),
            closure.blob_bindings.size(),
            static_cast<std::size_t>(normalized_traces.size()),
            std::size_t{2},
            static_cast<std::size_t>(authority_values.size()),
        },
    };
}

namespace {

constexpr qsizetype maximum_handoff_bytes = 16 * 1024 * 1024;
constexpr qsizetype maximum_declaration_template_bytes = 1024 * 1024;
constexpr qsizetype maximum_completed_declaration_bytes = 2 * 1024 * 1024;

[[nodiscard]] auto independentFailure(IndependentReviewErrorCode code, QString message)
    -> std::unexpected<IndependentReviewError> {
    return std::unexpected(IndependentReviewError{code, std::move(message), std::nullopt});
}

[[nodiscard]] auto independentCatalogFailure(const CatalogError& error)
    -> std::unexpected<IndependentReviewError> {
    return std::unexpected(IndependentReviewError{IndependentReviewErrorCode::CatalogFailure,
                                                  error.message, error.code});
}

[[nodiscard]] bool isLowercaseSha256(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{64}$"));
    return pattern.match(value).hasMatch();
}

[[nodiscard]] bool isExactInteger(const QJsonValue& value, int minimum, int maximum) {
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    return std::isfinite(number) && number >= static_cast<double>(minimum) &&
           number <= static_cast<double>(maximum) && std::trunc(number) == number;
}

[[nodiscard]] bool isAscii(const QString& value) {
    return std::ranges::all_of(value, [](QChar unit) { return unit.unicode() <= 0x7fU; });
}

[[nodiscard]] QJsonObject declarationTemplate() {
    QJsonObject dimensions;
    for (const auto* dimension_name : dimension_names) {
        dimensions.insert(QString::fromLatin1(dimension_name), QJsonValue::Null);
    }
    return QJsonObject{
        {QStringLiteral("declaration_kind"), QStringLiteral("independent_realism_review")},
        {QStringLiteral("dimensions"), dimensions},
        {QStringLiteral("handoff_digest"), QJsonValue::Null},
        {QStringLiteral("known_uncertainty"), QJsonValue::Null},
        {QStringLiteral("review_pack_id"), QJsonValue::Null},
        {QStringLiteral("review_pack_version"), QJsonValue::Null},
        {QStringLiteral("review_resource_id"), QJsonValue::Null},
        {QStringLiteral("review_state"), QJsonValue::Null},
        {QStringLiteral("reviewed_on"), QJsonValue::Null},
        {QStringLiteral("reviewer"),
         QJsonObject{
             {QStringLiteral("affiliation"), QJsonValue::Null},
             {QStringLiteral("display_name"), QJsonValue::Null},
             {QStringLiteral("qualification"), QJsonValue::Null},
             {QStringLiteral("reviewer_id"), QJsonValue::Null},
         }},
        {QStringLiteral("reviewer_reference"), QJsonValue::Null},
        {QStringLiteral("schema_version"), 1},
    };
}

[[nodiscard]] QString independentHandoffDigest(const QJsonObject& payload) {
    const auto compact = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-independent-realism-review-handoff-v1"));
    addUint64(hash, 1);
    addFrame(hash, QStringLiteral("independent_realism_review"));
    addFrame(hash, QByteArrayView(compact));
    return QString::fromLatin1(hash.result().toHex());
}

struct DetachedMechanicalContext final {
    model::PackRevision subject_revision;
    QString case_id;
    QString source_review_resource_id;
    QString source_review_path;
    QString source_review_sha256;
    QString source_reviewed_on;
    QJsonObject source_dimensions;
    QJsonArray source_uncertainty;
    QJsonObject mechanical_evidence;
    QHash<QString, QSet<QString>> complete_dimension_groups;
    QSet<QString> subject_pack_ids;
    QSet<QString> subject_resource_ids;
    bool has_argument_configuration{};
    bool has_bench_configuration{};
    bool has_judge_profile{};
    RealismEvidenceCounts counts;
};

[[nodiscard]] auto reconstructDetachedMechanicalContext(const ResolvedPack& resolved,
                                                        const QString& requested_case_id,
                                                        const QDate& current_utc_date,
                                                        IndependentReviewErrorCode failure_code)
    -> std::expected<DetachedMechanicalContext, IndependentReviewError> {
    const auto invalid = [failure_code](QString message) {
        return independentFailure(failure_code, std::move(message));
    };
    const auto& root = resolved.root();
    if (!current_utc_date.isValid() || root.manifest_schema_version != 2 ||
        requested_case_id.isEmpty() || !hasCapability(root, "workbench.pack.realism-evidence", 1)) {
        return invalid(QStringLiteral("The subject root is not eligible for detached review"));
    }

    qsizetype root_case_count = 0;
    for (const auto& resource : root.resources) {
        if (resource.descriptor.kind == model::ResourceKind::Case &&
            QString::fromStdString(resource.descriptor.id) == requested_case_id) {
            ++root_case_count;
        }
    }
    if (root_case_count != 1) {
        return invalid(QStringLiteral("The requested case is not owned exactly once by the root"));
    }

    std::vector<const LoadedPack*> dependencies;
    dependencies.reserve(resolved.dependenciesDependencyFirst().size());
    for (const auto& dependency : resolved.dependenciesDependencyFirst()) {
        dependencies.push_back(&dependency);
    }
    if (const auto validated = PackReader::validateResolvedGraph(
            root, std::span<const LoadedPack* const>(dependencies));
        !validated) {
        return invalid(
            QStringLiteral("The resolved subject is invalid: %1").arg(validated.error().message));
    }

    std::vector<const ValidatedResource*> reviews;
    for (const auto& resource : root.resources) {
        if (resource.descriptor.kind == model::ResourceKind::RealismReview &&
            resource.descriptor.schema_version == 2 &&
            resource.document.value(QStringLiteral("case_id")).toString() == requested_case_id) {
            reviews.push_back(&resource);
        }
    }
    if (reviews.size() != 1) {
        return invalid(QStringLiteral("The subject root must own exactly one schema-2 review for "
                                      "the requested case"));
    }
    const auto& review = *reviews.front();
    const auto review_state = review.document.value(QStringLiteral("review_state")).toString();
    const auto reviewed_on = review.document.value(QStringLiteral("reviewed_on")).toString();
    const auto parsed_reviewed_on = QDate::fromString(reviewed_on, Qt::ISODate);
    if (review_state != QStringLiteral("independent_review_pending") ||
        !parsed_reviewed_on.isValid() || parsed_reviewed_on.toString(Qt::ISODate) != reviewed_on ||
        parsed_reviewed_on > current_utc_date ||
        !review.document.value(QStringLiteral("evidence")).isObject()) {
        return invalid(QStringLiteral("The source review is not a complete, current pending "
                                      "independent-review source"));
    }

    const auto dimensions = review.document.value(QStringLiteral("dimensions")).toObject();
    if (!hasExactKeys(dimensions,
                      {"procedural_law", "deadlines_authority", "record_consistency",
                       "consequences", "oral_argument", "bench_differentiation", "provenance"})) {
        return invalid(QStringLiteral("The source review dimensions have an invalid shape"));
    }
    for (const auto* dimension_name : dimension_names) {
        if (!isExactInteger(dimensions.value(QString::fromLatin1(dimension_name)), 0, 2)) {
            return invalid(QStringLiteral("The source review dimensions must be integers from 0 "
                                          "through 2"));
        }
    }
    const auto uncertainty_value = review.document.value(QStringLiteral("known_uncertainty"));
    if (!uncertainty_value.isArray() ||
        !std::ranges::all_of(uncertainty_value.toArray(),
                             [](const QJsonValue& value) { return value.isObject(); })) {
        return invalid(QStringLiteral("The source review must use typed uncertainty objects"));
    }

    const auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    const auto source_traces = evidence.value(QStringLiteral("traces")).toArray();
    if (source_traces.isEmpty() || source_traces.size() > maximum_authoring_trace_count) {
        return invalid(QStringLiteral("The source review must contain from 1 through 256 traces"));
    }
    std::optional<std::pair<QString, QString>> previous_trace_key;
    QSet<QString> trace_evidence_refs;
    for (const auto& value : source_traces) {
        if (!value.isObject()) {
            return invalid(QStringLiteral("The source trace array is invalid"));
        }
        const auto source_trace = value.toObject();
        const auto key = std::pair{source_trace.value(QStringLiteral("trace_id")).toString(),
                                   source_trace.value(QStringLiteral("evidence_id")).toString()};
        if (source_trace.value(QStringLiteral("engine_revision")).toString() !=
                multiTraceAuthoringEngineRevision() ||
            key.first.isEmpty() || key.second.isEmpty() ||
            (previous_trace_key.has_value() && !(previous_trace_key.value() < key))) {
            return invalid(QStringLiteral("The source traces are not in the strict production-"
                                          "multi order"));
        }
        previous_trace_key = key;
        trace_evidence_refs.insert(key.second);
    }
    if (trace_evidence_refs.size() != source_traces.size()) {
        return invalid(QStringLiteral("The source trace evidence IDs are not unique"));
    }

    std::vector<const LoadedPack*> all_packs = dependencies;
    all_packs.push_back(&root);
    const auto closure_result = buildSubjectClosure(review, requested_case_id, all_packs);
    if (!closure_result || closure_result->case_owner != &root) {
        return invalid(!closure_result
                           ? QStringLiteral("Cannot reconstruct the source closure: %1")
                                 .arg(closure_result.error().message)
                           : QStringLiteral("The requested case is not rooted in the subject"));
    }
    auto closure = *closure_result;
    if (closure.subject_packs_dependency_first.size() > maximum_detached_subject_revision_count) {
        return invalid(QStringLiteral("The source closure leaves no detached revision headroom"));
    }
    std::size_t descriptor_count = 0;
    for (const auto* pack : closure.subject_packs_dependency_first) {
        if (pack->resources.size() > maximum_detached_subject_descriptor_count - descriptor_count) {
            return invalid(
                QStringLiteral("The source closure leaves no detached descriptor headroom"));
        }
        descriptor_count += pack->resources.size();
        if (pack->blobs.size() > maximum_detached_subject_descriptor_count - descriptor_count) {
            return invalid(
                QStringLiteral("The source closure leaves no detached descriptor headroom"));
        }
        descriptor_count += pack->blobs.size();
    }

    const auto profile_result = buildAuthoringEvidenceProfile(closure, requested_case_id);
    if (!profile_result) {
        return invalid(QStringLiteral("Cannot reconstruct the source evidence profile: %1")
                           .arg(profile_result.error()));
    }
    const auto& profile = *profile_result;

    QJsonArray pack_values;
    for (const auto& binding : closure.pack_bindings) {
        pack_values.push_back(packBindingObject(binding));
    }
    QHash<QString, QString> resource_evidence_by_id;
    QJsonArray resource_values;
    for (auto& binding : closure.resource_bindings) {
        binding.evidence_id =
            evidenceId(QStringLiteral("resource"),
                       {binding.owner_pack_id, binding.owner_pack_version, binding.resource_id,
                        binding.resource_kind, QString::number(binding.schema_version),
                        binding.path, binding.sha256});
        resource_evidence_by_id.insert(binding.resource_id, binding.evidence_id);
        resource_values.push_back(resourceBindingObject(binding));
    }
    QHash<QString, QString> blob_evidence_by_owner_path;
    QJsonArray blob_values;
    for (auto& binding : closure.blob_bindings) {
        binding.evidence_id =
            evidenceId(QStringLiteral("blob"),
                       {binding.owner_pack_id, binding.owner_pack_version, binding.path,
                        binding.media_type, QString::number(binding.byte_size), binding.sha256});
        blob_evidence_by_owner_path.insert(binding.owner_pack_id + u'\n' + binding.path,
                                           binding.evidence_id);
        blob_values.push_back(blobBindingObject(binding));
    }

    const auto projected = loadRuntimePackForEvidence(*closure.case_owner,
                                                      closure.subject_packs_dependency_first, true);
    if (!projected) {
        return invalid(QStringLiteral("The exact subject cannot be projected for independent trace "
                                      "replay: %1")
                           .arg(QString::fromStdString(projected.error().message)));
    }
    const auto runtime_case = std::ranges::find(
        projected->cases, requested_case_id.toStdString(),
        [](const RuntimeCase& candidate) { return candidate.definition.id.value; });
    if (runtime_case == projected->cases.end()) {
        return invalid(QStringLiteral("The reviewed case is absent from the runtime projection"));
    }
    QJsonArray normalized_source_traces;
    QJsonArray detached_traces;
    trace_evidence_refs.clear();
    for (const auto& value : source_traces) {
        const auto source_trace = value.toObject();
        const auto normalized =
            normalizeExecutedTrace(review, requested_case_id, source_trace, *runtime_case);
        if (!normalized || *normalized != source_trace) {
            return invalid(
                !normalized ? QStringLiteral("A source trace cannot be independently replayed: %1")
                                  .arg(normalized.error().message)
                            : QStringLiteral("A source trace is not replay-normalized"));
        }
        normalized_source_traces.push_back(*normalized);
        trace_evidence_refs.insert(normalized->value(QStringLiteral("evidence_id")).toString());
        auto detached_trace = *normalized;
        detached_trace.insert(QStringLiteral("engine_revision"), detachedReviewEngineRevision());
        detached_trace.insert(QStringLiteral("digest"),
                              traceDigest(requested_case_id, detached_trace));
        detached_traces.push_back(detached_trace);
    }

    QSet<QString> record_blob_refs;
    const auto record_id =
        closure.case_resource->document.value(QStringLiteral("record_id")).toString();
    const auto record = closure.resources_by_id.find(record_id.toStdString());
    if (record == closure.resources_by_id.end() ||
        record->second.resource->descriptor.kind != model::ResourceKind::Record) {
        return invalid(QStringLiteral("The reviewed record is absent from the source closure"));
    }
    const ResourceBinding record_binding{
        {},
        QString::fromStdString(record->second.owner->revision.id.value),
        QString::fromStdString(record->second.owner->revision.version),
        QString::fromStdString(record->second.resource->descriptor.id),
        kindName(record->second.resource->descriptor.kind),
        record->second.resource->descriptor.schema_version,
        QString::fromStdString(record->second.resource->descriptor.path),
        QString::fromStdString(record->second.resource->descriptor.sha256),
    };
    std::vector<BlobBinding> record_blobs;
    for (const auto& entry :
         record->second.resource->document.value(QStringLiteral("docket_entries")).toArray()) {
        const auto path = entry.toObject().value(QStringLiteral("asset_path")).toString();
        const auto blob = closure.blobs_by_owner_path.find(record->second.owner->revision.id.value +
                                                           '\n' + path.toStdString());
        if (blob == closure.blobs_by_owner_path.end()) {
            return invalid(QStringLiteral("A reviewed record asset is absent from the source "
                                          "closure"));
        }
        record_blobs.push_back(blob->second);
        const auto reference = blob_evidence_by_owner_path.value(
            QString::fromStdString(record->second.owner->revision.id.value) + u'\n' + path);
        if (!reference.isEmpty()) {
            record_blob_refs.insert(reference);
        }
    }
    const auto make_record_check = [&](const QString& kind, std::vector<BlobBinding> blobs) {
        QJsonObject check{
            {QStringLiteral("evidence_id"),
             evidenceId(QStringLiteral("record-check"), {requested_case_id, kind})},
            {QStringLiteral("check_id"), recordCheckId(kind, requested_case_id)},
            {QStringLiteral("record_id"), record_id},
            {QStringLiteral("check_kind"), kind},
        };
        check.insert(QStringLiteral("digest"),
                     recordCheckDigest(requested_case_id, check, record_binding, std::move(blobs)));
        return check;
    };
    const auto asset_check = make_record_check(QStringLiteral("asset_resolution"), record_blobs);
    const auto anchor_check = make_record_check(QStringLiteral("page_anchor_resolution"), {});
    const QJsonArray record_checks{asset_check, anchor_check};
    const QSet<QString> record_check_refs{
        asset_check.value(QStringLiteral("evidence_id")).toString(),
        anchor_check.value(QStringLiteral("evidence_id")).toString(),
    };

    QJsonArray authority_values;
    QHash<QString, QString> authority_evidence_by_id;
    auto authority_ids = profile.all_authority_ids.values();
    std::ranges::sort(authority_ids);
    for (const auto& authority_id : authority_ids) {
        const auto binding_id =
            evidenceId(QStringLiteral("authority"), {requested_case_id, authority_id});
        authority_evidence_by_id.insert(authority_id, binding_id);
        authority_values.push_back(QJsonObject{
            {QStringLiteral("evidence_id"), binding_id},
            {QStringLiteral("authority_id"), authority_id},
        });
    }
    const AuthoringEvidenceBindings bindings{
        resource_evidence_by_id, authority_evidence_by_id, record_blob_refs,
        record_check_refs,       trace_evidence_refs,
    };
    const auto complete_groups = authoringDimensionGroups(profile, bindings);
    QJsonObject complete_dimension_evidence;
    QJsonObject source_dimension_evidence;
    for (const auto* dimension_name : dimension_names) {
        const auto key = QString::fromLatin1(dimension_name);
        const auto complete = complete_groups.value(key);
        if (complete.size() > maximum_dimension_evidence_reference_count) {
            return invalid(QStringLiteral("The latent %1 evidence partition exceeds 512 "
                                          "references")
                               .arg(key));
        }
        auto conditioned = complete;
        if (dimensions.value(key).toInt() == 0) {
            conditioned.clear();
        } else if (conditioned.isEmpty()) {
            return invalid(
                QStringLiteral("The source %1 evidence partition is not exact").arg(key));
        }
        complete_dimension_evidence.insert(key, sortedReferences(complete));
        source_dimension_evidence.insert(key, sortedReferences(conditioned));
    }

    const auto rebuilt_closure_digest = closureDigest(
        requested_case_id, closure.pack_bindings, closure.resource_bindings, closure.blob_bindings);
    const QJsonObject rebuilt_source_evidence{
        {QStringLiteral("authorities"), authority_values},
        {QStringLiteral("blobs"), blob_values},
        {QStringLiteral("closure_digest"), rebuilt_closure_digest},
        {QStringLiteral("dimension_evidence"), source_dimension_evidence},
        {QStringLiteral("packs"), pack_values},
        {QStringLiteral("record_checks"), record_checks},
        {QStringLiteral("resources"), resource_values},
        {QStringLiteral("traces"), normalized_source_traces},
    };
    if (rebuilt_source_evidence != evidence) {
        return invalid(QStringLiteral("The source evidence differs from the independent canonical "
                                      "catalog reconstruction"));
    }
    QJsonObject mechanical_evidence{
        {QStringLiteral("authorities"), authority_values},
        {QStringLiteral("blobs"), blob_values},
        {QStringLiteral("closure_digest"), rebuilt_closure_digest},
        {QStringLiteral("dimension_evidence"), complete_dimension_evidence},
        {QStringLiteral("packs"), pack_values},
        {QStringLiteral("record_checks"), record_checks},
        {QStringLiteral("resources"), resource_values},
        {QStringLiteral("traces"), detached_traces},
    };

    DetachedMechanicalContext context{
        root.revision,
        requested_case_id,
        QString::fromStdString(review.descriptor.id),
        QString::fromStdString(review.descriptor.path),
        QString::fromStdString(review.descriptor.sha256),
        reviewed_on,
        dimensions,
        uncertainty_value.toArray(),
        std::move(mechanical_evidence),
        complete_groups,
        {},
        {},
        !profile.argument_resource_ids.isEmpty(),
        !profile.bench_resource_ids.isEmpty(),
        !profile.judge_resource_ids.isEmpty(),
        RealismEvidenceCounts{
            static_cast<std::size_t>(evidence.value(QStringLiteral("packs")).toArray().size()),
            static_cast<std::size_t>(evidence.value(QStringLiteral("resources")).toArray().size()),
            static_cast<std::size_t>(evidence.value(QStringLiteral("blobs")).toArray().size()),
            static_cast<std::size_t>(source_traces.size()),
            static_cast<std::size_t>(
                evidence.value(QStringLiteral("record_checks")).toArray().size()),
            static_cast<std::size_t>(
                evidence.value(QStringLiteral("authorities")).toArray().size()),
        },
    };
    for (const auto& revision : resolved.revisionsByPackId()) {
        context.subject_pack_ids.insert(QString::fromStdString(revision.id.value));
    }
    for (const auto* pack : all_packs) {
        for (const auto& resource : pack->resources) {
            context.subject_resource_ids.insert(QString::fromStdString(resource.descriptor.id));
        }
    }
    return context;
}

struct IndependentReviewClaims final {
    QString review_pack_id;
    QString review_pack_version;
    QString review_resource_id;
    QJsonObject dimensions;
    QJsonArray known_uncertainty;
    QString reviewed_on;
    QJsonObject reviewer;
    QString reviewer_reference;
};

[[nodiscard]] auto buildDetachedReviewPack(const ResolvedPack& subject,
                                           const DetachedMechanicalContext& context,
                                           const IndependentReviewClaims& claims,
                                           IndependentReviewErrorCode failure_code)
    -> std::expected<FinalizedIndependentReview, IndependentReviewError> {
    const auto invalid = [failure_code](QString message) {
        return independentFailure(failure_code, std::move(message));
    };

    auto evidence = context.mechanical_evidence;
    auto dimension_evidence = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    for (const auto* dimension_name : dimension_names) {
        const auto key = QString::fromLatin1(dimension_name);
        if (claims.dimensions.value(key).toInt() == 0) {
            dimension_evidence.insert(key, QJsonArray{});
        }
    }
    evidence.insert(QStringLiteral("dimension_evidence"), dimension_evidence);

    auto final_reviewer = claims.reviewer;
    if (final_reviewer.value(QStringLiteral("affiliation")).isNull()) {
        final_reviewer.remove(QStringLiteral("affiliation"));
    }
    const QJsonObject review_document{
        {QStringLiteral("case_id"), context.case_id},
        {QStringLiteral("dimensions"), claims.dimensions},
        {QStringLiteral("evidence"), evidence},
        {QStringLiteral("known_uncertainty"), claims.known_uncertainty},
        {QStringLiteral("resource_id"), claims.review_resource_id},
        {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
        {QStringLiteral("review_state"), QStringLiteral("independently_reviewed")},
        {QStringLiteral("reviewed_on"), claims.reviewed_on},
        {QStringLiteral("reviewer"), final_reviewer},
        {QStringLiteral("reviewer_reference"), claims.reviewer_reference},
        {QStringLiteral("schema_version"), 2},
    };

    const auto validator = SchemaValidator::fromBundledSchemas(2);
    if (!validator) {
        return invalid(QStringLiteral("Cannot load the schema-2 validator: %1")
                           .arg(validator.error().message));
    }
    if (const auto validated =
            validator->validate(QStringLiteral("realism-review.schema.json"), review_document);
        !validated) {
        return invalid(QStringLiteral("The generated detached review is invalid: %1")
                           .arg(validated.error().message));
    }
    const auto review_bytes = serializedObject(review_document);
    if (review_bytes.size() > maximum_authoring_review_bytes) {
        return invalid(QStringLiteral("The generated detached review exceeds 8 MiB"));
    }
    const auto parsed_review = SchemaValidator::parseObject(
        QByteArrayView(review_bytes), QStringLiteral("resources/realism-review.json"),
        JsonLimits{64, 200'000});
    if (!parsed_review || *parsed_review != review_document ||
        !hasOnlyUnicodeScalars(*parsed_review)) {
        return invalid(QStringLiteral("The generated detached review does not pass the ordinary "
                                      "JSON boundary"));
    }
    const auto review_sha256 = sha256(QByteArrayView(review_bytes));

    const QJsonObject manifest{
        {QStringLiteral("blobs"), QJsonArray{}},
        {QStringLiteral("contents"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), claims.review_resource_id},
             {QStringLiteral("kind"), QStringLiteral("realism_review")},
             {QStringLiteral("path"), QStringLiteral("resources/realism-review.json")},
             {QStringLiteral("schema_version"), 2},
             {QStringLiteral("sha256"), review_sha256},
         }}},
        {QStringLiteral("dependencies"),
         QJsonArray{QJsonObject{
             {QStringLiteral("pack_id"), QString::fromStdString(context.subject_revision.id.value)},
             {QStringLiteral("sha256"), QString::fromStdString(context.subject_revision.digest)},
             {QStringLiteral("version"), QString::fromStdString(context.subject_revision.version)},
         }}},
        {QStringLiteral("pack_id"), claims.review_pack_id},
        {QStringLiteral("required_capabilities"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                 {QStringLiteral("version"), 2}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.realism-evidence")},
                         {QStringLiteral("version"), 1}},
         }},
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("version"), claims.review_pack_version},
    };
    if (const auto validated =
            validator->validate(QStringLiteral("manifest.schema.json"), manifest);
        !validated) {
        return invalid(QStringLiteral("The generated detached manifest is invalid: %1")
                           .arg(validated.error().message));
    }
    const auto manifest_bytes = serializedObject(manifest);
    if (manifest_bytes.size() > maximum_authoring_manifest_bytes) {
        return invalid(QStringLiteral("The generated detached manifest exceeds 1 MiB"));
    }
    const auto parsed_manifest = SchemaValidator::parseObject(QByteArrayView(manifest_bytes),
                                                              QStringLiteral("manifest.json"));
    if (!parsed_manifest || *parsed_manifest != manifest) {
        return invalid(QStringLiteral("The generated detached manifest does not pass the ordinary "
                                      "JSON boundary"));
    }

    LoadedPack detached{
        2,
        model::PackRevision{model::PackId{claims.review_pack_id.toStdString()},
                            claims.review_pack_version.toStdString(),
                            {}},
        std::vector<model::RequiredCapability>{
            model::RequiredCapability{"workbench.pack.declarative-resources", 2},
            model::RequiredCapability{"workbench.pack.realism-evidence", 1},
        },
        std::vector<model::PackDependency>{model::PackDependency{context.subject_revision}},
        std::vector<ValidatedResource>{ValidatedResource{
            model::DeclarativeResource{
                model::ResourceKind::RealismReview, claims.review_resource_id.toStdString(), 2,
                "resources/realism-review.json", review_sha256.toStdString()},
            review_document,
        }},
        {},
        {},
        PackGraphState::DeferredReferences,
    };
    detached.revision.digest = recomputedPackRevisionDigest(detached).toStdString();

    std::vector<const LoadedPack*> dependency_first;
    dependency_first.reserve(subject.dependenciesDependencyFirst().size() + 1U);
    for (const auto& dependency : subject.dependenciesDependencyFirst()) {
        dependency_first.push_back(&dependency);
    }
    dependency_first.push_back(&subject.root());
    if (const auto validated = PackReader::validateResolvedGraph(
            detached, std::span<const LoadedPack* const>(dependency_first));
        !validated) {
        return invalid(QStringLiteral("The generated detached pack fails resolved validation: %1")
                           .arg(validated.error().message));
    }

    return FinalizedIndependentReview{
        detached.revision,
        context.subject_revision,
        context.case_id,
        claims.review_resource_id,
        review_sha256,
        context.mechanical_evidence.value(QStringLiteral("closure_digest")).toString(),
        {},
        review_document,
        manifest,
        review_bytes,
        manifest_bytes,
    };
}

[[nodiscard]] const std::vector<QString>& minimumNamespacedIdCandidates() {
    static const auto candidates = [] {
        constexpr std::string_view alphabet{"-.0123456789abcdefghijklmnopqrstuvwxyz"};
        std::vector<QString> result;
        result.reserve(10'000);
        std::string candidate;
        std::function<void(std::size_t)> append;
        for (std::size_t length = 3; result.size() < 10'000; ++length) {
            candidate.assign(length, '-');
            append = [&](std::size_t offset) {
                if (result.size() >= 10'000) {
                    return;
                }
                if (offset == candidate.size()) {
                    const auto value = QString::fromLatin1(candidate);
                    if (isNamespacedId(value) && value.toUtf8().size() <= 128) {
                        result.push_back(value);
                    }
                    return;
                }
                for (const auto character : alphabet) {
                    candidate.at(offset) = character;
                    append(offset + 1U);
                    if (result.size() >= 10'000) {
                        return;
                    }
                }
            };
            append(0);
        }
        return result;
    }();
    return candidates;
}

[[nodiscard]] std::optional<QString> firstAvailableMinimumId(const QSet<QString>& occupied,
                                                             std::size_t maximum_candidates) {
    const auto& candidates = minimumNamespacedIdCandidates();
    const auto inspected = std::min(maximum_candidates, candidates.size());
    for (std::size_t index = 0; index < inspected; ++index) {
        if (!occupied.contains(candidates.at(index))) {
            return candidates.at(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto validateMinimumDetachedPack(const ResolvedPack& subject,
                                               const DetachedMechanicalContext& context,
                                               IndependentReviewErrorCode failure_code)
    -> std::expected<void, IndependentReviewError> {
    const auto pack_id = firstAvailableMinimumId(context.subject_pack_ids, 128);
    const auto resource_id = firstAvailableMinimumId(context.subject_resource_ids, 10'000);
    if (!pack_id || !resource_id) {
        return independentFailure(failure_code,
                                  QStringLiteral("Cannot allocate minimum detached feasibility "
                                                 "identifiers within the bounded search"));
    }
    QJsonObject dimensions;
    for (const auto* dimension_name : dimension_names) {
        dimensions.insert(QString::fromLatin1(dimension_name), 0);
    }
    const IndependentReviewClaims claims{
        *pack_id,
        QStringLiteral("0.0.0"),
        *resource_id,
        dimensions,
        {},
        context.source_reviewed_on,
        QJsonObject{
            {QStringLiteral("affiliation"), QJsonValue::Null},
            {QStringLiteral("display_name"), QStringLiteral("x")},
            {QStringLiteral("qualification"), QStringLiteral("x")},
            {QStringLiteral("reviewer_id"), QStringLiteral("a.a")},
        },
        QStringLiteral("x"),
    };
    const auto built = buildDetachedReviewPack(subject, context, claims, failure_code);
    if (!built) {
        return std::unexpected(built.error());
    }
    return {};
}

[[nodiscard]] auto composePreparedIndependentReview(const DetachedMechanicalContext& context)
    -> std::expected<PreparedIndependentReview, IndependentReviewError> {
    const auto declaration_template = declarationTemplate();
    const auto template_bytes = serializedObject(declaration_template);
    if (template_bytes.size() > maximum_declaration_template_bytes) {
        return independentFailure(IndependentReviewErrorCode::InvalidReviewSource,
                                  QStringLiteral("The declaration template exceeds 1 MiB"));
    }
    const auto parsed_template = SchemaValidator::parseObject(
        QByteArrayView(template_bytes), QStringLiteral("review-declaration.template.json"),
        JsonLimits{32, 4'096});
    if (!parsed_template || *parsed_template != declaration_template) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidReviewSource,
            QStringLiteral("The declaration template does not pass its JSON boundary"));
    }
    const auto template_sha256 = sha256(QByteArrayView(template_bytes));
    const QJsonObject source_review{
        {QStringLiteral("dimensions"), context.source_dimensions},
        {QStringLiteral("known_uncertainty"), context.source_uncertainty},
        {QStringLiteral("path"), context.source_review_path},
        {QStringLiteral("resource_id"), context.source_review_resource_id},
        {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
        {QStringLiteral("review_state"), QStringLiteral("independent_review_pending")},
        {QStringLiteral("reviewed_on"), context.source_reviewed_on},
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("sha256"), context.source_review_sha256},
    };
    const QJsonObject subject_revision{
        {QStringLiteral("digest"), QString::fromStdString(context.subject_revision.digest)},
        {QStringLiteral("pack_id"), QString::fromStdString(context.subject_revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(context.subject_revision.version)},
    };
    const QJsonObject payload{
        {QStringLiteral("case_id"), context.case_id},
        {QStringLiteral("declaration_template_sha256"), template_sha256},
        {QStringLiteral("mechanical_evidence"), context.mechanical_evidence},
        {QStringLiteral("mechanical_trace_revision"), detachedReviewEngineRevision()},
        {QStringLiteral("source_review"), source_review},
        {QStringLiteral("subject_revision"), subject_revision},
    };
    const auto handoff_digest = independentHandoffDigest(payload);
    const QJsonObject handoff{
        {QStringLiteral("handoff_digest"), handoff_digest},
        {QStringLiteral("handoff_kind"), QStringLiteral("independent_realism_review")},
        {QStringLiteral("payload"), payload},
        {QStringLiteral("schema_version"), 1},
    };
    const auto handoff_bytes = serializedObject(handoff);
    if (handoff_bytes.size() > maximum_handoff_bytes) {
        return independentFailure(IndependentReviewErrorCode::InvalidReviewSource,
                                  QStringLiteral("The deterministic handoff exceeds 16 MiB"));
    }
    const auto parsed_handoff = SchemaValidator::parseObject(
        QByteArrayView(handoff_bytes), QStringLiteral("handoff.json"), JsonLimits{64, 500'000});
    if (!parsed_handoff || *parsed_handoff != handoff) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidReviewSource,
            QStringLiteral("The deterministic handoff does not pass its JSON boundary"));
    }
    return PreparedIndependentReview{
        context.subject_revision,
        context.case_id,
        context.source_review_resource_id,
        context.mechanical_evidence.value(QStringLiteral("closure_digest")).toString(),
        handoff_digest,
        handoff,
        declaration_template,
        handoff_bytes,
        template_bytes,
        context.counts,
    };
}

[[nodiscard]] auto parseIndependentObject(const QByteArray& bytes, qsizetype maximum_bytes,
                                          JsonLimits limits, QStringView name,
                                          IndependentReviewErrorCode failure_code,
                                          bool require_canonical)
    -> std::expected<QJsonObject, IndependentReviewError> {
    if (bytes.isEmpty() || bytes.size() > maximum_bytes) {
        return independentFailure(
            failure_code,
            QStringLiteral("%1 is empty or exceeds its byte limit").arg(name.toString()));
    }
    const auto decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.toUtf8() != bytes) {
        return independentFailure(failure_code,
                                  QStringLiteral("%1 is not valid UTF-8").arg(name.toString()));
    }
    const auto parsed = SchemaValidator::parseObject(QByteArrayView(bytes), name, limits);
    if (!parsed || !hasOnlyUnicodeScalars(*parsed)) {
        return independentFailure(
            failure_code,
            !parsed
                ? QStringLiteral("%1 is invalid: %2").arg(name.toString(), parsed.error().message)
                : QStringLiteral("%1 contains a non-scalar string").arg(name.toString()));
    }
    if (require_canonical && serializedObject(*parsed) != bytes) {
        return independentFailure(
            failure_code,
            QStringLiteral("%1 is not in the canonical indented form").arg(name.toString()));
    }
    return *parsed;
}

[[nodiscard]] auto validateIndependentReviewClaims(const QJsonObject& declaration,
                                                   const DetachedMechanicalContext& context,
                                                   const QString& expected_handoff_digest,
                                                   const QDate& current_utc_date)
    -> std::expected<IndependentReviewClaims, IndependentReviewError> {
    const auto invalid = [](QString message) {
        return independentFailure(IndependentReviewErrorCode::InvalidDeclaration,
                                  std::move(message));
    };
    if (!hasOnlyUnicodeScalars(declaration) ||
        !hasExactKeys(declaration, {"declaration_kind", "dimensions", "handoff_digest",
                                    "known_uncertainty", "review_pack_id", "review_pack_version",
                                    "review_resource_id", "review_state", "reviewed_on", "reviewer",
                                    "reviewer_reference", "schema_version"}) ||
        !isExactInteger(declaration.value(QStringLiteral("schema_version")), 1, 1) ||
        declaration.value(QStringLiteral("declaration_kind")).toString() !=
            QStringLiteral("independent_realism_review") ||
        declaration.value(QStringLiteral("handoff_digest")).toString() != expected_handoff_digest) {
        return invalid(QStringLiteral("The completed declaration has the wrong closed shape, "
                                      "kind, schema, or handoff association"));
    }

    const auto review_pack_id_value = declaration.value(QStringLiteral("review_pack_id"));
    const auto review_pack_version_value = declaration.value(QStringLiteral("review_pack_version"));
    const auto review_resource_id_value = declaration.value(QStringLiteral("review_resource_id"));
    if (!review_pack_id_value.isString() || !review_pack_version_value.isString() ||
        !review_resource_id_value.isString()) {
        return invalid(QStringLiteral("The completed declaration identities must be strings"));
    }
    const auto review_pack_id = review_pack_id_value.toString();
    const auto review_pack_version = review_pack_version_value.toString();
    const auto review_resource_id = review_resource_id_value.toString();
    const auto pack_id_bytes = review_pack_id.toUtf8();
    const auto resource_id_bytes = review_resource_id.toUtf8();
    if (!isNamespacedId(review_pack_id) || pack_id_bytes.size() < 3 || pack_id_bytes.size() > 128 ||
        context.subject_pack_ids.contains(review_pack_id) ||
        !isValidPackVersion(review_pack_version, 2) || review_pack_version.toUtf8().size() < 5 ||
        review_pack_version.toUtf8().size() > 128 || !isAscii(review_resource_id) ||
        !isNamespacedId(review_resource_id) || resource_id_bytes.size() < 3 ||
        resource_id_bytes.size() > 128 ||
        context.subject_resource_ids.contains(review_resource_id)) {
        return invalid(QStringLiteral("The completed declaration pack or resource identity is "
                                      "invalid or collides with the subject"));
    }
    if (!declaration.value(QStringLiteral("review_state")).isString() ||
        declaration.value(QStringLiteral("review_state")).toString() !=
            QStringLiteral("independently_reviewed")) {
        return invalid(QStringLiteral("The completed declaration review state is invalid"));
    }

    const auto reviewed_on_value = declaration.value(QStringLiteral("reviewed_on"));
    const auto reviewed_on = reviewed_on_value.toString();
    const auto parsed_reviewed_on = QDate::fromString(reviewed_on, Qt::ISODate);
    const auto source_date = QDate::fromString(context.source_reviewed_on, Qt::ISODate);
    if (!reviewed_on_value.isString() || !parsed_reviewed_on.isValid() ||
        parsed_reviewed_on.toString(Qt::ISODate) != reviewed_on ||
        parsed_reviewed_on < source_date || parsed_reviewed_on > current_utc_date) {
        return invalid(QStringLiteral("The completed declaration review date is outside the "
                                      "source/current UTC bounds"));
    }

    const auto reference_value = declaration.value(QStringLiteral("reviewer_reference"));
    if (!reference_value.isString() || !isTrimStableUtf8Text(reference_value.toString(), 512)) {
        return invalid(QStringLiteral("The completed reviewer reference is invalid"));
    }
    const auto reviewer_value = declaration.value(QStringLiteral("reviewer"));
    if (!reviewer_value.isObject()) {
        return invalid(QStringLiteral("The completed reviewer metadata is required"));
    }
    const auto reviewer = reviewer_value.toObject();
    if (!hasExactKeys(reviewer, {"affiliation", "display_name", "qualification", "reviewer_id"})) {
        return invalid(QStringLiteral("The completed reviewer metadata is not closed"));
    }
    const auto reviewer_id = reviewer.value(QStringLiteral("reviewer_id"));
    const auto display_name = reviewer.value(QStringLiteral("display_name"));
    const auto qualification = reviewer.value(QStringLiteral("qualification"));
    const auto affiliation = reviewer.value(QStringLiteral("affiliation"));
    if (!reviewer_id.isString() || !isNamespacedId(reviewer_id.toString()) ||
        reviewer_id.toString().toUtf8().size() < 3 ||
        reviewer_id.toString().toUtf8().size() > 160 || !display_name.isString() ||
        !isTrimStableUtf8Text(display_name.toString(), 240) || !qualification.isString() ||
        !isTrimStableUtf8Text(qualification.toString(), 1'024) ||
        (!affiliation.isNull() &&
         (!affiliation.isString() || !isTrimStableUtf8Text(affiliation.toString(), 240)))) {
        return invalid(QStringLiteral("The completed reviewer metadata violates its ID or UTF-8 "
                                      "text bounds"));
    }

    const auto dimensions_value = declaration.value(QStringLiteral("dimensions"));
    if (!dimensions_value.isObject()) {
        return invalid(QStringLiteral("The completed dimensions object is required"));
    }
    const auto dimensions = dimensions_value.toObject();
    if (!hasExactKeys(dimensions,
                      {"procedural_law", "deadlines_authority", "record_consistency",
                       "consequences", "oral_argument", "bench_differentiation", "provenance"})) {
        return invalid(QStringLiteral("The completed dimensions object is not closed"));
    }
    for (const auto* dimension_name : dimension_names) {
        const auto key = QString::fromLatin1(dimension_name);
        if (!isExactInteger(dimensions.value(key), 0, 3) ||
            (dimensions.value(key).toInt() != 0 &&
             context.complete_dimension_groups.value(key).isEmpty())) {
            return invalid(QStringLiteral("The completed %1 score is invalid or lacks mechanical "
                                          "support")
                               .arg(key));
        }
    }
    if (dimensions.value(QStringLiteral("oral_argument")).toInt() > 0 &&
        !context.has_argument_configuration) {
        return invalid(QStringLiteral("A nonzero oral-argument score lacks a case-targeted "
                                      "argument configuration"));
    }
    if (dimensions.value(QStringLiteral("bench_differentiation")).toInt() > 0 &&
        (!context.has_bench_configuration || !context.has_judge_profile)) {
        return invalid(QStringLiteral("A nonzero bench-differentiation score lacks its bench or "
                                      "judge prerequisite"));
    }

    const auto uncertainty_value = declaration.value(QStringLiteral("known_uncertainty"));
    if (!uncertainty_value.isArray() || uncertainty_value.toArray().size() > 256) {
        return invalid(QStringLiteral("Completed uncertainties must be an array of at most 256 "
                                      "typed items"));
    }
    QSet<QString> uncertainty_ids;
    for (const auto& value : uncertainty_value.toArray()) {
        if (!value.isObject()) {
            return invalid(QStringLiteral("Every completed uncertainty must be an object"));
        }
        const auto uncertainty = value.toObject();
        const auto blocking_value = uncertainty.value(QStringLiteral("blocking"));
        const auto blocking = blocking_value.isBool() && blocking_value.toBool();
        const auto has_remediation = uncertainty.contains(QStringLiteral("remediation_issue"));
        if (!blocking_value.isBool() ||
            (!has_remediation &&
             !hasExactKeys(uncertainty, {"blocking", "summary", "uncertainty_id"})) ||
            (has_remediation && !hasExactKeys(uncertainty, {"blocking", "remediation_issue",
                                                            "summary", "uncertainty_id"})) ||
            blocking != has_remediation) {
            return invalid(QStringLiteral("A completed uncertainty has the wrong closed shape"));
        }
        const auto id = uncertainty.value(QStringLiteral("uncertainty_id"));
        const auto summary = uncertainty.value(QStringLiteral("summary"));
        if (!id.isString() || !isNamespacedId(id.toString()) ||
            id.toString().toUtf8().size() > 160 || uncertainty_ids.contains(id.toString()) ||
            !summary.isString() || !isTrimStableUtf8Text(summary.toString(), 2'048)) {
            return invalid(QStringLiteral("A completed uncertainty ID or summary is invalid"));
        }
        uncertainty_ids.insert(id.toString());
        if (blocking) {
            const auto remediation = uncertainty.value(QStringLiteral("remediation_issue"));
            const auto remediation_bytes = remediation.toString().toUtf8();
            if (!remediation.isString() ||
                !model::isCanonicalAuthoritySourceUrl(
                    std::string_view(remediation_bytes.constData(),
                                     static_cast<std::size_t>(remediation_bytes.size())))) {
                return invalid(QStringLiteral("A blocking uncertainty remediation issue is not a "
                                              "canonical HTTPS URL"));
            }
        }
    }

    return IndependentReviewClaims{
        review_pack_id, review_pack_version,         review_resource_id,
        dimensions,     uncertainty_value.toArray(), reviewed_on,
        reviewer,       reference_value.toString(),
    };
}

} // namespace

std::expected<PreparedIndependentReview, IndependentReviewError>
prepareIndependentReview(const PackCatalogSnapshot& snapshot,
                         const IndependentReviewPrepareInput& input) {
    const auto pack_id = QString::fromStdString(input.subject_revision.id.value);
    const auto version = QString::fromStdString(input.subject_revision.version);
    const auto digest = QString::fromStdString(input.subject_revision.digest);
    if (!input.current_utc_date.isValid() || !isNamespacedId(pack_id) ||
        pack_id.toUtf8().size() > 128 || !isValidPackVersion(version, 2) ||
        !isLowercaseSha256(digest) || !isNamespacedId(input.case_id)) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidInput,
            QStringLiteral("Prepare requires a valid captured UTC date, exact schema-2 subject "
                           "revision, and case ID"));
    }
    const auto resolved = snapshot.loadResolved(input.subject_revision);
    if (!resolved) {
        return independentCatalogFailure(resolved.error());
    }
    const auto context =
        reconstructDetachedMechanicalContext(*resolved, input.case_id, input.current_utc_date,
                                             IndependentReviewErrorCode::InvalidReviewSource);
    if (!context) {
        return std::unexpected(context.error());
    }
    if (const auto feasible = validateMinimumDetachedPack(
            *resolved, *context, IndependentReviewErrorCode::InvalidReviewSource);
        !feasible) {
        return std::unexpected(feasible.error());
    }

    const auto first = composePreparedIndependentReview(*context);
    if (!first) {
        return std::unexpected(first.error());
    }
    const auto second = composePreparedIndependentReview(*context);
    if (!second || first->handoff_bytes != second->handoff_bytes ||
        first->declaration_template_bytes != second->declaration_template_bytes ||
        first->handoff_digest != second->handoff_digest || first->counts != second->counts) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidReviewSource,
            QStringLiteral("The prepare builder did not reproduce byte-identical output"));
    }
    return *first;
}

std::expected<FinalizedIndependentReview, IndependentReviewError>
finalizeIndependentReview(const PackCatalogSnapshot& snapshot,
                          const IndependentReviewFinalizeInput& input) {
    if (!input.current_utc_date.isValid()) {
        return independentFailure(IndependentReviewErrorCode::InvalidInput,
                                  QStringLiteral("Finalize requires a valid captured UTC date"));
    }
    const auto handoff = parseIndependentObject(
        input.handoff_bytes, maximum_handoff_bytes, JsonLimits{64, 500'000},
        QStringLiteral("handoff.json"), IndependentReviewErrorCode::InvalidHandoff, true);
    if (!handoff) {
        return std::unexpected(handoff.error());
    }
    const auto parsed_template = parseIndependentObject(
        input.declaration_template_bytes, maximum_declaration_template_bytes, JsonLimits{32, 4'096},
        QStringLiteral("review-declaration.template.json"),
        IndependentReviewErrorCode::InvalidHandoff, true);
    if (!parsed_template) {
        return std::unexpected(parsed_template.error());
    }
    const auto declaration = parseIndependentObject(
        input.completed_declaration_bytes, maximum_completed_declaration_bytes,
        JsonLimits{32, 4'096}, QStringLiteral("completed declaration"),
        IndependentReviewErrorCode::InvalidDeclaration, false);
    if (!declaration) {
        return std::unexpected(declaration.error());
    }

    if (*parsed_template != declarationTemplate() ||
        !hasExactKeys(*handoff, {"handoff_digest", "handoff_kind", "payload", "schema_version"}) ||
        !isExactInteger(handoff->value(QStringLiteral("schema_version")), 1, 1) ||
        handoff->value(QStringLiteral("handoff_kind")).toString() !=
            QStringLiteral("independent_realism_review") ||
        !handoff->value(QStringLiteral("payload")).isObject() ||
        !handoff->value(QStringLiteral("handoff_digest")).isString() ||
        !isLowercaseSha256(handoff->value(QStringLiteral("handoff_digest")).toString())) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidHandoff,
            QStringLiteral("The handoff or declaration template has the wrong closed shape"));
    }
    const auto payload = handoff->value(QStringLiteral("payload")).toObject();
    const auto template_sha256 = sha256(QByteArrayView(input.declaration_template_bytes));
    if (!hasExactKeys(payload,
                      {"case_id", "declaration_template_sha256", "mechanical_evidence",
                       "mechanical_trace_revision", "source_review", "subject_revision"}) ||
        !payload.value(QStringLiteral("case_id")).isString() ||
        !payload.value(QStringLiteral("declaration_template_sha256")).isString() ||
        !payload.value(QStringLiteral("mechanical_evidence")).isObject() ||
        !payload.value(QStringLiteral("mechanical_trace_revision")).isString() ||
        !payload.value(QStringLiteral("source_review")).isObject() ||
        !payload.value(QStringLiteral("subject_revision")).isObject() ||
        payload.value(QStringLiteral("declaration_template_sha256")).toString() !=
            template_sha256 ||
        payload.value(QStringLiteral("mechanical_trace_revision")).toString() !=
            detachedReviewEngineRevision() ||
        independentHandoffDigest(payload) !=
            handoff->value(QStringLiteral("handoff_digest")).toString()) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidHandoff,
            QStringLiteral("The handoff payload shape or association digest is invalid"));
    }
    const auto subject_value = payload.value(QStringLiteral("subject_revision")).toObject();
    const auto pack_id = subject_value.value(QStringLiteral("pack_id")).toString();
    const auto version = subject_value.value(QStringLiteral("version")).toString();
    const auto case_id = payload.value(QStringLiteral("case_id")).toString();
    if (!hasExactKeys(subject_value, {"digest", "pack_id", "version"}) ||
        !subject_value.value(QStringLiteral("digest")).isString() ||
        !subject_value.value(QStringLiteral("pack_id")).isString() ||
        !subject_value.value(QStringLiteral("version")).isString() || !isNamespacedId(pack_id) ||
        pack_id.toUtf8().size() > 128 || !isValidPackVersion(version, 2) ||
        !isNamespacedId(case_id) ||
        !isLowercaseSha256(subject_value.value(QStringLiteral("digest")).toString())) {
        return independentFailure(IndependentReviewErrorCode::InvalidHandoff,
                                  QStringLiteral("The handoff subject revision is invalid"));
    }
    const model::PackRevision subject_revision{
        model::PackId{pack_id.toStdString()},
        version.toStdString(),
        subject_value.value(QStringLiteral("digest")).toString().toStdString(),
    };
    const auto resolved = snapshot.loadResolved(subject_revision);
    if (!resolved) {
        return independentCatalogFailure(resolved.error());
    }
    const auto context = reconstructDetachedMechanicalContext(
        *resolved, case_id, input.current_utc_date, IndependentReviewErrorCode::InvalidHandoff);
    if (!context) {
        return std::unexpected(context.error());
    }
    if (const auto feasible = validateMinimumDetachedPack(
            *resolved, *context, IndependentReviewErrorCode::InvalidHandoff);
        !feasible) {
        return std::unexpected(feasible.error());
    }
    const auto rebuilt = composePreparedIndependentReview(*context);
    if (!rebuilt || rebuilt->handoff_bytes != input.handoff_bytes ||
        rebuilt->declaration_template_bytes != input.declaration_template_bytes) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidHandoff,
            !rebuilt ? QStringLiteral("The handoff cannot be independently reconstructed: %1")
                           .arg(rebuilt.error().message)
                     : QStringLiteral("The handoff differs from the current exact subject"));
    }

    const auto claims = validateIndependentReviewClaims(
        *declaration, *context, rebuilt->handoff_digest, input.current_utc_date);
    if (!claims) {
        return std::unexpected(claims.error());
    }
    const auto first = buildDetachedReviewPack(
        *resolved, *context, *claims, IndependentReviewErrorCode::InvalidIndependentReviewPack);
    if (!first) {
        return std::unexpected(first.error());
    }
    const auto second = buildDetachedReviewPack(
        *resolved, *context, *claims, IndependentReviewErrorCode::InvalidIndependentReviewPack);
    if (!second || first->review_bytes != second->review_bytes ||
        first->manifest_bytes != second->manifest_bytes || first->revision != second->revision) {
        return independentFailure(
            IndependentReviewErrorCode::InvalidIndependentReviewPack,
            QStringLiteral("The finalize builder did not reproduce byte-identical output"));
    }

    const auto installed = snapshot.load(first->revision.id, first->revision.version);
    if (installed && installed->revision.digest != first->revision.digest) {
        return independentFailure(
            IndependentReviewErrorCode::ImmutableConflict,
            QStringLiteral("The catalog already contains a different immutable revision for %1 "
                           "%2")
                .arg(QString::fromStdString(first->revision.id.value),
                     QString::fromStdString(first->revision.version)));
    }
    if (!installed && installed.error().code != CatalogErrorCode::NotFound) {
        return independentCatalogFailure(installed.error());
    }
    auto result = *first;
    result.handoff_digest = rebuilt->handoff_digest;
    return result;
}

} // namespace appellate::packs
