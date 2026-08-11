#include "realism_evidence.hpp"
#include "runtime_pack_internal.hpp"

#include "appellate/engine/workflow_engine.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace appellate::packs {
namespace {

constexpr std::array dimension_names{
    "procedural_law", "deadlines_authority",   "record_consistency", "consequences",
    "oral_argument",  "bench_differentiation", "provenance",
};

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

[[nodiscard]] std::expected<void, Error>
validateExecutedTrace(const ValidatedResource& review, const QString& case_id,
                      const QJsonObject& trace, const RuntimeCase& runtime_case) {
    const auto journal_values = trace.value(QStringLiteral("journal")).toArray();
    const auto command_count =
        static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toDouble());
    const auto declared_event_count =
        static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toDouble());
    if (static_cast<std::uint64_t>(journal_values.size()) != command_count) {
        return fail(review, QStringLiteral("evidence/traces/journal"),
                    QStringLiteral("journal entry count differs from command_count"));
    }

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
    if (event_count != declared_event_count ||
        trace.value(QStringLiteral("journal_sha256")).toString() != computed_journal_sha256 ||
        trace.value(QStringLiteral("operation_ids")).toArray() != executed_operation_ids) {
        return fail(review, QStringLiteral("evidence/traces/journal"),
                    QStringLiteral("journal bytes differ from declared counts, digest, or events"));
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
            runtime_case.workflow.id.value ||
        trace.value(QStringLiteral("terminal_stage_id")).toString().toStdString() !=
            replayed->current_stage_id.value ||
        trace.value(QStringLiteral("digest")).toString() != traceDigest(case_id, trace)) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("replayed trace identity, terminal state, or digest is stale"));
    }
    return {};
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

[[nodiscard]] bool isPinnedLegacyV2Review(const ValidatedResource& review,
                                          const LoadedPack& pack) {
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
    for (const auto& value : evidence.value(QStringLiteral("resources")).toArray()) {
        const auto object = value.toObject();
        if (!addEvidenceId(object)) {
            return fail(review, QStringLiteral("evidence/resources/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        auto binding = parseResource(object);
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
    for (const auto& value : evidence.value(QStringLiteral("blobs")).toArray()) {
        const auto object = value.toObject();
        if (!addEvidenceId(object)) {
            return fail(review, QStringLiteral("evidence/blobs/evidence_id"),
                        QStringLiteral("evidence IDs must be globally unique"));
        }
        auto binding = parseBlob(object);
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

    const auto projected = loadRuntimePackForEvidence(
        *closure.case_owner, closure.subject_packs_dependency_first);
    if (!projected) {
        return fail(review, QStringLiteral("evidence/traces"),
                    QStringLiteral("the exact subject cannot be projected for replay: %1")
                        .arg(QString::fromStdString(projected.error().message)));
    }
    const auto runtime_case =
        std::ranges::find(projected->cases, case_id.toStdString(), [](const RuntimeCase& candidate) {
            return candidate.definition.id.value;
        });
    if (runtime_case == projected->cases.end()) {
        return fail(review, QStringLiteral("case_id"),
                    QStringLiteral("the projected subject case is absent"));
    }
    QSet<QString> trace_ids;
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
        const auto replayed = validateExecutedTrace(review, case_id, trace, *runtime_case);
        if (!replayed) {
            return std::unexpected(replayed.error());
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
    }
    QSet<QString> record_check_ids;
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

} // namespace appellate::packs
