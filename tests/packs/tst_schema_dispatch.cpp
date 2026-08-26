#include "../../src/packs/src/runtime_pack_internal.hpp"
#include "appellate/engine/workflow_engine.hpp"
#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"
#include "appellate/storage/workflow_codec.hpp"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using appellate::packs::ErrorCode;
using appellate::packs::PackReader;

class SchemaDispatchTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesPinnedV1Digests();
    void preservesPinnedPre27V2Revision();
    void preservesPinnedPre28V2Revision();
    void preservesPinnedPre25V2Revision();
    void loadsV2AndProjectsRuntime();
    void allowsProcedureAuthoritySetsBeyondCourtDefaults();
    void validatesSealedRecordTwins();
    void validatesGroundedQuestionBanks();
    void normalizesGroundedQuestionBankOrdering();
    void enforcesGroundedQuestionBoundsAndPrompts();
    void rejectsUnknownAndMismatchedCapabilities();
    void rejectsUnderdeclaredCapabilities();
    void preservesLegacyV2WithoutOptionalFeatures();
    void rejectsUnderdeclaredDispositionAndPreconditionCapabilities();
    void validatesClosedWorkflowPreconditions();
    void validatesDependentDeadlineBasesAndReachedCondition();
    void validatesNamedEventDateAndArgumentDateFeatures();
    void validatesStructuredDispositionPlans();
    void enforcesStructuredFeatureBounds();
    void closureAndRuntimeRejectForgedCapabilityCoverage();
    void rejectsInvalidCanonicalAuthorityMetadata();
    void rejectsUnresolvedAndDuplicateAuthoritySelections();
    void resolvesCanonicalAuthorityAcrossDependencyGraph();
    void validatesExactRealismEvidenceAndReviewExclusion();
    void rejectsTamperedAndIncompleteRealismEvidence();
    void enforcesDetachedIndependentReviewOwnership();
    void enforcesDetachedIndependentReviewHumanShape();
    void enforcesCodeOwnedRealismScorePrerequisites();
    void enforcesCodeOwnedRealismUnicodeScalars();
    void rejectsUnsupportedKindVersions();
    void rejectsV1V2CrossInterpretation();
    void validatesSharedWorkflowCapabilitySchemas();
};

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

[[nodiscard]] bool writeBytes(const QString& path, const QByteArray& bytes) {
    const QFileInfo info(path);
    if (!QDir{}.mkpath(info.absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

[[nodiscard]] bool copyTree(const QString& source, const QString& destination) {
    const QDir root(source);
    QDirIterator iterator(source, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source_path = iterator.next();
        QFile file(source_path);
        if (!file.open(QIODevice::ReadOnly) ||
            !writeBytes(QDir(destination).filePath(root.relativeFilePath(source_path)),
                        file.readAll())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool copyPre25V2Fixture(const QString& destination) {
    if (!copyTree(fixture(QStringLiteral("full-resource-pack-v2")), destination) ||
        !copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-27-overlay")), destination) ||
        !copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-25-overlay")), destination)) {
        return false;
    }
    return QFile::remove(
        QDir(destination)
            .filePath(QStringLiteral("resources/argument-config-counterfactual.json")));
}

[[nodiscard]] bool copyPre27V2Fixture(const QString& destination) {
    return copyTree(fixture(QStringLiteral("full-resource-pack-v2")), destination) &&
           copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-27-overlay")), destination);
}

[[nodiscard]] QJsonObject readObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

[[nodiscard]] bool writeObject(const QString& path, const QJsonObject& object) {
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
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

[[nodiscard]] QString dispositionDigest(const QJsonObject& case_document, const QJsonObject& plan) {
    std::vector<QJsonObject> components;
    for (const auto& value : plan.value(QStringLiteral("components")).toArray()) {
        components.push_back(value.toObject());
    }
    std::ranges::sort(components, [](const QJsonObject& left, const QJsonObject& right) {
        return std::tuple{left.value(QStringLiteral("issue_id")).toString(),
                          left.value(QStringLiteral("target_id")).toString()} <
               std::tuple{right.value(QStringLiteral("issue_id")).toString(),
                          right.value(QStringLiteral("target_id")).toString()};
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-disposition-plan-v1"));
    addFrame(hash, case_document.value(QStringLiteral("resource_id")).toString());
    addFrame(hash, case_document.value(QStringLiteral("authored_disposition_id")).toString());
    addFrame(hash, plan.value(QStringLiteral("plan_id")).toString());
    addFrame(hash, plan.value(QStringLiteral("finality")).toString());
    addUint64(hash, static_cast<std::uint64_t>(components.size()));
    for (const auto& component : components) {
        addFrame(hash, component.value(QStringLiteral("issue_id")).toString());
        addFrame(hash, component.value(QStringLiteral("target_id")).toString());
        addFrame(hash, component.value(QStringLiteral("scope")).toString());
        addFrame(hash, component.value(QStringLiteral("action")).toString());
        addUint64(hash, component.value(QStringLiteral("remand")).toBool() ? 1U : 0U);
        for (const auto& field :
             {QStringLiteral("authority_ids"), QStringLiteral("record_anchor_ids")}) {
            std::vector<QString> ids;
            for (const auto& value : component.value(field).toArray()) {
                ids.push_back(value.toString());
            }
            std::ranges::sort(ids);
            addUint64(hash, static_cast<std::uint64_t>(ids.size()));
            for (const auto& id : ids) {
                addFrame(hash, id);
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString questionBankDigest(const QJsonObject& configuration, const QJsonObject& bank,
                                         const QJsonObject& authority_set,
                                         const QJsonObject& record) {
    QHash<QString, QJsonObject> authorities;
    for (const auto& value : authority_set.value(QStringLiteral("authorities")).toArray()) {
        const auto authority = value.toObject();
        authorities.insert(authority.value(QStringLiteral("authority_id")).toString(), authority);
    }
    QHash<QString, QJsonObject> entries;
    for (const auto& value : record.value(QStringLiteral("docket_entries")).toArray()) {
        const auto entry = value.toObject();
        entries.insert(entry.value(QStringLiteral("entry_id")).toString(), entry);
    }
    QHash<QString, QJsonObject> anchors;
    for (const auto& value : record.value(QStringLiteral("page_anchors")).toArray()) {
        const auto anchor = value.toObject();
        anchors.insert(anchor.value(QStringLiteral("anchor_id")).toString(), anchor);
    }

    std::vector<QJsonObject> bindings;
    for (const auto& value : bank.value(QStringLiteral("issue_topic_bindings")).toArray()) {
        bindings.push_back(value.toObject());
    }
    std::ranges::sort(bindings, [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("issue_id")).toString() <
               right.value(QStringLiteral("issue_id")).toString();
    });
    std::vector<QJsonObject> questions;
    for (const auto& value : bank.value(QStringLiteral("questions")).toArray()) {
        questions.push_back(value.toObject());
    }
    std::ranges::sort(questions, [](const QJsonObject& left, const QJsonObject& right) {
        return left.value(QStringLiteral("question_id")).toString() <
               right.value(QStringLiteral("question_id")).toString();
    });

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-grounded-question-bank-v1"));
    addFrame(hash, configuration.value(QStringLiteral("case_id")).toString());
    addFrame(hash, configuration.value(QStringLiteral("resource_id")).toString());
    addFrame(hash, bank.value(QStringLiteral("mode")).toString());
    addUint64(hash, static_cast<std::uint64_t>(bindings.size()));
    for (const auto& binding : bindings) {
        addFrame(hash, binding.value(QStringLiteral("issue_id")).toString());
        std::vector<QString> topics;
        for (const auto& value : binding.value(QStringLiteral("topic_ids")).toArray()) {
            topics.push_back(value.toString());
        }
        std::ranges::sort(topics);
        addUint64(hash, static_cast<std::uint64_t>(topics.size()));
        for (const auto& topic : topics) {
            addFrame(hash, topic);
        }
    }
    addUint64(hash, static_cast<std::uint64_t>(questions.size()));
    for (const auto& question : questions) {
        addFrame(hash, question.value(QStringLiteral("question_id")).toString());
        addFrame(hash, question.value(QStringLiteral("issue_id")).toString());
        addFrame(hash, question.value(QStringLiteral("topic_id")).toString());
        addFrame(hash, question.value(QStringLiteral("prompt")).toString());
        std::vector<QJsonObject> grounding;
        for (const auto& value : question.value(QStringLiteral("grounding")).toArray()) {
            grounding.push_back(value.toObject());
        }
        std::ranges::sort(grounding, [](const QJsonObject& left, const QJsonObject& right) {
            return left.value(QStringLiteral("grounding_id")).toString() <
                   right.value(QStringLiteral("grounding_id")).toString();
        });
        addUint64(hash, static_cast<std::uint64_t>(grounding.size()));
        for (const auto& reference : grounding) {
            const auto kind = reference.value(QStringLiteral("kind")).toString();
            addFrame(hash, reference.value(QStringLiteral("grounding_id")).toString());
            addFrame(hash, kind);
            if (kind == QStringLiteral("authority")) {
                const auto authority =
                    authorities.value(reference.value(QStringLiteral("authority_id")).toString());
                for (const auto& field :
                     {QStringLiteral("authority_id"), QStringLiteral("citation"),
                      QStringLiteral("source_version"), QStringLiteral("proposition")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash, 1U);
                for (const auto& field :
                     {QStringLiteral("authority_type"), QStringLiteral("jurisdiction_id"),
                      QStringLiteral("issuing_body_id"), QStringLiteral("precedential_status")}) {
                    addFrame(hash, authority.value(field).toString());
                }
                addUint64(hash,
                          authority.value(QStringLiteral("official_source")).toBool() ? 1U : 0U);
                for (const auto& field : {QStringLiteral("checked_on"), QStringLiteral("locator"),
                                          QStringLiteral("source_url")}) {
                    addFrame(hash, authority.value(field).toString());
                }
            } else if (kind == QStringLiteral("brief_page")) {
                const auto entry_id = reference.value(QStringLiteral("entry_id")).toString();
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<std::uint64_t>(
                                    reference.value(QStringLiteral("page_number")).toInt()));
                addFrame(hash,
                         entries.value(entry_id).value(QStringLiteral("asset_sha256")).toString());
            } else {
                const auto anchor_id = reference.value(QStringLiteral("anchor_id")).toString();
                const auto anchor = anchors.value(anchor_id);
                const auto entry_id = anchor.value(QStringLiteral("entry_id")).toString();
                addFrame(hash, anchor_id);
                addFrame(hash, entry_id);
                addUint64(hash, static_cast<std::uint64_t>(
                                    anchor.value(QStringLiteral("page_number")).toInt()));
                addFrame(hash,
                         entries.value(entry_id).value(QStringLiteral("asset_sha256")).toString());
                const auto has_citation = anchor.contains(QStringLiteral("citation_label"));
                addUint64(hash, has_citation ? 1U : 0U);
                if (has_citation) {
                    addFrame(hash, anchor.value(QStringLiteral("citation_label")).toString());
                }
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

void refreshDispositionDigests(QJsonObject& case_document) {
    auto plans = case_document.value(QStringLiteral("disposition_plans")).toArray();
    for (qsizetype index = 0; index < plans.size(); ++index) {
        auto plan = plans.at(index).toObject();
        plan.insert(QStringLiteral("digest"), dispositionDigest(case_document, plan));
        plans.replace(index, plan);
    }
    case_document.insert(QStringLiteral("disposition_plans"), plans);
}

[[nodiscard]] bool mutateManifest(const QString& root,
                                  const std::function<void(QJsonObject&)>& mutation) {
    const auto path = QDir(root).filePath(QStringLiteral("manifest.json"));
    auto manifest = readObject(path);
    if (manifest.isEmpty()) {
        return false;
    }
    mutation(manifest);
    return writeObject(path, manifest);
}

[[nodiscard]] bool setResourceSchemaVersion(const QString& root, const QString& relative_path,
                                            int schema_version) {
    const auto resource_path = QDir(root).filePath(relative_path);
    auto resource = readObject(resource_path);
    if (resource.isEmpty()) {
        return false;
    }
    resource.insert(QStringLiteral("schema_version"), schema_version);
    const auto bytes = QJsonDocument(resource).toJson(QJsonDocument::Compact);
    if (!writeBytes(resource_path, bytes)) {
        return false;
    }
    const auto digest =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return mutateManifest(root, [&](QJsonObject& manifest) {
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        for (qsizetype index = 0; index < contents.size(); ++index) {
            auto descriptor = contents.at(index).toObject();
            if (descriptor.value(QStringLiteral("path")).toString() == relative_path) {
                descriptor.insert(QStringLiteral("sha256"), digest);
                contents.replace(index, descriptor);
                break;
            }
        }
        manifest.insert(QStringLiteral("contents"), contents);
    });
}

[[nodiscard]] bool mutateResource(const QString& root, const QString& relative_path,
                                  const std::function<void(QJsonObject&)>& mutation) {
    const auto resource_path = QDir(root).filePath(relative_path);
    auto resource = readObject(resource_path);
    if (resource.isEmpty()) {
        return false;
    }
    mutation(resource);
    const auto bytes = QJsonDocument(resource).toJson(QJsonDocument::Compact);
    if (!writeBytes(resource_path, bytes)) {
        return false;
    }
    const auto digest =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return mutateManifest(root, [&](QJsonObject& manifest) {
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        for (qsizetype index = 0; index < contents.size(); ++index) {
            auto descriptor = contents.at(index).toObject();
            if (descriptor.value(QStringLiteral("path")).toString() == relative_path) {
                descriptor.insert(QStringLiteral("sha256"), digest);
                contents.replace(index, descriptor);
                break;
            }
        }
        manifest.insert(QStringLiteral("contents"), contents);
    });
}

[[nodiscard]] bool addCaseAuthoritySet(const QString& root) {
    const auto authority_set_path = QStringLiteral("resources/authority-set-case.json");
    const auto source =
        readObject(QDir(root).filePath(QStringLiteral("resources/authority-set.json")));
    auto authorities = source.value(QStringLiteral("authorities")).toArray();
    if (source.isEmpty() || authorities.isEmpty()) {
        return false;
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
    auto case_authority_set = source;
    case_authority_set.insert(QStringLiteral("resource_id"),
                              QStringLiteral("example.authorities.case-specific"));
    case_authority_set.insert(QStringLiteral("authorities"), QJsonArray{authority});
    const auto bytes = QJsonDocument(case_authority_set).toJson(QJsonDocument::Compact);
    if (!writeBytes(QDir(root).filePath(authority_set_path), bytes) ||
        !mutateManifest(root, [&](QJsonObject& manifest) {
            auto contents = manifest.value(QStringLiteral("contents")).toArray();
            contents.push_back(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("example.authorities.case-specific")},
                {QStringLiteral("kind"), QStringLiteral("authority_set")},
                {QStringLiteral("schema_version"), 2},
                {QStringLiteral("path"), authority_set_path},
                {QStringLiteral("sha256"),
                 QString::fromLatin1(
                     QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            });
            manifest.insert(QStringLiteral("contents"), contents);
        })) {
        return false;
    }
    return mutateResource(root, QStringLiteral("resources/procedure-profile.json"),
                          [](QJsonObject& procedure) {
                              auto sets =
                                  procedure.value(QStringLiteral("authority_set_ids")).toArray();
                              sets.push_back(QStringLiteral("example.authorities.case-specific"));
                              procedure.insert(QStringLiteral("authority_set_ids"), sets);
                          }) &&
           mutateResource(root, QStringLiteral("resources/case.json"), [](QJsonObject& case_) {
               auto issues = case_.value(QStringLiteral("issues")).toArray();
               auto issue = issues.first().toObject();
               auto issue_authority_ids = issue.value(QStringLiteral("authority_ids")).toArray();
               issue_authority_ids.push_back(QStringLiteral("example.authority.case-specific"));
               issue.insert(QStringLiteral("authority_ids"), issue_authority_ids);
               issues.replace(0, issue);
               case_.insert(QStringLiteral("issues"), issues);
           });
}

[[nodiscard]] bool refreshQuestionBankDigest(const QString& root, const QString& argument_path) {
    const auto configuration = readObject(QDir(root).filePath(argument_path));
    const auto authorities =
        readObject(QDir(root).filePath(QStringLiteral("resources/authority-set.json")));
    const auto record = readObject(QDir(root).filePath(QStringLiteral("resources/record.json")));
    const auto bank = configuration.value(QStringLiteral("grounded_question_bank")).toObject();
    if (configuration.isEmpty() || authorities.isEmpty() || record.isEmpty() || bank.isEmpty()) {
        return false;
    }
    const auto digest = questionBankDigest(configuration, bank, authorities, record);
    return mutateResource(root, argument_path, [&digest](QJsonObject& document) {
        auto updated = document.value(QStringLiteral("grounded_question_bank")).toObject();
        updated.insert(QStringLiteral("grounding_digest"), digest);
        document.insert(QStringLiteral("grounded_question_bank"), updated);
    });
}

[[nodiscard]] bool stripGroundedQuestions(const QString& root) {
    for (const auto& path : {QStringLiteral("resources/argument-config.json"),
                             QStringLiteral("resources/argument-config-counterfactual.json")}) {
        if (!mutateResource(root, path, [](QJsonObject& document) {
                document.remove(QStringLiteral("grounded_question_bank"));
            })) {
            return false;
        }
    }
    return mutateManifest(root, [](QJsonObject& manifest) {
        QJsonArray capabilities;
        for (const auto& value :
             manifest.value(QStringLiteral("required_capabilities")).toArray()) {
            if (value.toObject().value(QStringLiteral("id")).toString() !=
                QStringLiteral("workbench.pack.grounded-questions")) {
                capabilities.push_back(value);
            }
        }
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    });
}

[[nodiscard]] bool addSealedRecordTwins(const QString& root) {
    if (!mutateResource(root, QStringLiteral("resources/record.json"), [](QJsonObject& document) {
            auto entries = document.value(QStringLiteral("docket_entries")).toArray();
            auto sealed = entries.at(0).toObject();
            sealed.insert(QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed"));
            sealed.insert(QStringLiteral("entry_number"), 3);
            sealed.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 42-S"));
            sealed.insert(QStringLiteral("title"),
                          QStringLiteral("Confidential PSR fixture title"));
            sealed.insert(QStringLiteral("description"),
                          QStringLiteral("Secret fixture description"));
            sealed.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("psr-secret-tag")});
            sealed.insert(QStringLiteral("sealed"), true);
            entries.push_back(sealed);
            document.insert(QStringLiteral("docket_entries"), entries);

            auto anchors = document.value(QStringLiteral("page_anchors")).toArray();
            anchors.push_back(QJsonObject{
                {QStringLiteral("anchor_id"), QStringLiteral("example.record.anchor.psr-sealed")},
                {QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed")},
                {QStringLiteral("page_number"), 2},
                {QStringLiteral("citation_label"), QStringLiteral("SECRET-JA-2")},
            });
            anchors.push_back(QJsonObject{
                {QStringLiteral("anchor_id"), QStringLiteral("example.record.anchor.psr-unmapped")},
                {QStringLiteral("entry_id"), QStringLiteral("example.record.psr-sealed")},
                {QStringLiteral("page_number"), 1},
                {QStringLiteral("citation_label"), QStringLiteral("SECRET-JA-UNMAPPED")},
            });
            document.insert(QStringLiteral("page_anchors"), anchors);
            document.insert(
                QStringLiteral("disclosure_policy"),
                QJsonObject{
                    {QStringLiteral("policy_id"), QStringLiteral("example.record.policy.psr")},
                    {QStringLiteral("unauthorized_projection"),
                     QStringLiteral("public_counterparts_only")},
                    {QStringLiteral("authorized_projection"),
                     QStringLiteral("public_and_authorized_sealed")},
                    {QStringLiteral("sealed_asset_access"),
                     QStringLiteral("session_event_grant_required")},
                });
            document.insert(
                QStringLiteral("sealed_disclosures"),
                QJsonArray{QJsonObject{
                    {QStringLiteral("disclosure_id"), QStringLiteral("example.disclosure.psr")},
                    {QStringLiteral("sealed_entry_id"),
                     QStringLiteral("example.record.psr-sealed")},
                    {QStringLiteral("public_entry_id"), QStringLiteral("example.record.entry-one")},
                    {QStringLiteral("authorization_authority_id"),
                     QStringLiteral("example.authority.rule-one")},
                    {QStringLiteral("required_items"),
                     QJsonArray{QStringLiteral("motion"), QStringLiteral("certificate"),
                                QStringLiteral("redacted_counterpart")}},
                    {QStringLiteral("anchor_mappings"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("stable_anchor_id"),
                          QStringLiteral("example.record.anchor.psr-stable")},
                         {QStringLiteral("sealed_anchor_id"),
                          QStringLiteral("example.record.anchor.psr-sealed")},
                         {QStringLiteral("public_anchor_id"),
                          QStringLiteral("example.record.anchor.ja2")},
                     }}},
                }});
        })) {
        return false;
    }
    return mutateManifest(root, [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        capabilities.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("workbench.pack.sealed-record-twins")},
            {QStringLiteral("version"), 1},
        });
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    });
}

[[nodiscard]] std::vector<QJsonObject>
objects(const QJsonArray& values,
        const std::function<bool(const QJsonObject&, const QJsonObject&)>& less) {
    std::vector<QJsonObject> result;
    result.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& value : values) {
        result.push_back(value.toObject());
    }
    std::ranges::sort(result, less);
    return result;
}

[[nodiscard]] bool realismCapabilityLess(const QJsonObject& left, const QJsonObject& right) {
    return std::tuple{left.value(QStringLiteral("id")).toString(),
                      left.value(QStringLiteral("version")).toInt()} <
           std::tuple{right.value(QStringLiteral("id")).toString(),
                      right.value(QStringLiteral("version")).toInt()};
}

[[nodiscard]] bool realismDependencyLess(const QJsonObject& left, const QJsonObject& right) {
    return std::tuple{left.value(QStringLiteral("pack_id")).toString(),
                      left.value(QStringLiteral("version")).toString()} <
           std::tuple{right.value(QStringLiteral("pack_id")).toString(),
                      right.value(QStringLiteral("version")).toString()};
}

[[nodiscard]] bool realismPackLess(const QJsonObject& left, const QJsonObject& right) {
    return std::tuple{left.value(QStringLiteral("pack_id")).toString(),
                      left.value(QStringLiteral("version")).toString()} <
           std::tuple{right.value(QStringLiteral("pack_id")).toString(),
                      right.value(QStringLiteral("version")).toString()};
}

[[nodiscard]] bool realismResourceLess(const QJsonObject& left, const QJsonObject& right) {
    return std::tuple{left.value(QStringLiteral("owner_pack_id")).toString(),
                      left.value(QStringLiteral("owner_pack_version")).toString(),
                      left.value(QStringLiteral("resource_id")).toString(),
                      left.value(QStringLiteral("resource_kind")).toString(),
                      left.value(QStringLiteral("schema_version")).toInt(),
                      left.value(QStringLiteral("path")).toString(),
                      left.value(QStringLiteral("sha256")).toString(),
                      left.value(QStringLiteral("evidence_id")).toString()} <
           std::tuple{right.value(QStringLiteral("owner_pack_id")).toString(),
                      right.value(QStringLiteral("owner_pack_version")).toString(),
                      right.value(QStringLiteral("resource_id")).toString(),
                      right.value(QStringLiteral("resource_kind")).toString(),
                      right.value(QStringLiteral("schema_version")).toInt(),
                      right.value(QStringLiteral("path")).toString(),
                      right.value(QStringLiteral("sha256")).toString(),
                      right.value(QStringLiteral("evidence_id")).toString()};
}

[[nodiscard]] bool realismBlobLess(const QJsonObject& left, const QJsonObject& right) {
    return std::tuple{left.value(QStringLiteral("owner_pack_id")).toString(),
                      left.value(QStringLiteral("owner_pack_version")).toString(),
                      left.value(QStringLiteral("path")).toString(),
                      left.value(QStringLiteral("media_type")).toString(),
                      left.value(QStringLiteral("byte_size")).toInteger(),
                      left.value(QStringLiteral("sha256")).toString(),
                      left.value(QStringLiteral("evidence_id")).toString()} <
           std::tuple{right.value(QStringLiteral("owner_pack_id")).toString(),
                      right.value(QStringLiteral("owner_pack_version")).toString(),
                      right.value(QStringLiteral("path")).toString(),
                      right.value(QStringLiteral("media_type")).toString(),
                      right.value(QStringLiteral("byte_size")).toInteger(),
                      right.value(QStringLiteral("sha256")).toString(),
                      right.value(QStringLiteral("evidence_id")).toString()};
}

void addRealismPackBinding(QCryptographicHash& hash, const QJsonObject& binding) {
    addFrame(hash, binding.value(QStringLiteral("pack_id")).toString());
    addFrame(hash, binding.value(QStringLiteral("version")).toString());
    addUint64(hash, static_cast<std::uint64_t>(
                        binding.value(QStringLiteral("manifest_schema_version")).toInt()));
    const auto capabilities = objects(
        binding.value(QStringLiteral("required_capabilities")).toArray(), realismCapabilityLess);
    addUint64(hash, capabilities.size());
    for (const auto& capability : capabilities) {
        addFrame(hash, capability.value(QStringLiteral("id")).toString());
        addUint64(hash,
                  static_cast<std::uint64_t>(capability.value(QStringLiteral("version")).toInt()));
    }
    const auto dependencies =
        objects(binding.value(QStringLiteral("dependencies")).toArray(), realismDependencyLess);
    addUint64(hash, dependencies.size());
    for (const auto& dependency : dependencies) {
        addFrame(hash, dependency.value(QStringLiteral("pack_id")).toString());
        addFrame(hash, dependency.value(QStringLiteral("version")).toString());
    }
}

void addRealismResourceDescriptor(QCryptographicHash& hash, const QJsonObject& binding) {
    addFrame(hash, binding.value(QStringLiteral("owner_pack_id")).toString());
    addFrame(hash, binding.value(QStringLiteral("owner_pack_version")).toString());
    addFrame(hash, binding.value(QStringLiteral("resource_id")).toString());
    addFrame(hash, binding.value(QStringLiteral("resource_kind")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(binding.value(QStringLiteral("schema_version")).toInt()));
    addFrame(hash, binding.value(QStringLiteral("path")).toString());
    addFrame(hash, binding.value(QStringLiteral("sha256")).toString());
}

void addRealismResourceBinding(QCryptographicHash& hash, const QJsonObject& binding) {
    addFrame(hash, binding.value(QStringLiteral("evidence_id")).toString());
    addRealismResourceDescriptor(hash, binding);
}

void addRealismBlobDescriptor(QCryptographicHash& hash, const QJsonObject& binding) {
    addFrame(hash, binding.value(QStringLiteral("owner_pack_id")).toString());
    addFrame(hash, binding.value(QStringLiteral("owner_pack_version")).toString());
    addFrame(hash, binding.value(QStringLiteral("path")).toString());
    addFrame(hash, binding.value(QStringLiteral("media_type")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(binding.value(QStringLiteral("byte_size")).toInteger()));
    addFrame(hash, binding.value(QStringLiteral("sha256")).toString());
}

void addRealismBlobBinding(QCryptographicHash& hash, const QJsonObject& binding) {
    addFrame(hash, binding.value(QStringLiteral("evidence_id")).toString());
    addRealismBlobDescriptor(hash, binding);
}

[[nodiscard]] QString realismClosureDigest(const QString& case_id, const QJsonArray& pack_values,
                                           const QJsonArray& resource_values,
                                           const QJsonArray& blob_values) {
    const auto packs = objects(pack_values, realismPackLess);
    const auto resources = objects(resource_values, realismResourceLess);
    const auto blobs = objects(blob_values, realismBlobLess);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-case-evidence-closure-v1"));
    addFrame(hash, case_id);
    addUint64(hash, packs.size());
    for (const auto& pack : packs) {
        addRealismPackBinding(hash, pack);
    }
    addUint64(hash, resources.size());
    for (const auto& resource : resources) {
        addRealismResourceBinding(hash, resource);
    }
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addRealismBlobBinding(hash, blob);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString realismTraceDigest(const QString& case_id, const QJsonObject& trace) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-executed-trace-evidence-v1"));
    addFrame(hash, case_id);
    addFrame(hash, trace.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("trace_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("workflow_id")).toString());
    addFrame(hash, trace.value(QStringLiteral("engine_revision")).toString());
    addUint64(hash,
              static_cast<std::uint64_t>(trace.value(QStringLiteral("command_count")).toInt()));
    addUint64(hash, static_cast<std::uint64_t>(trace.value(QStringLiteral("event_count")).toInt()));
    addFrame(hash, trace.value(QStringLiteral("journal_sha256")).toString());
    const auto operation_ids = trace.value(QStringLiteral("operation_ids")).toArray();
    addUint64(hash, static_cast<std::uint64_t>(operation_ids.size()));
    for (const auto& operation_id : operation_ids) {
        addFrame(hash, operation_id.toString());
    }
    addFrame(hash, trace.value(QStringLiteral("terminal_stage_id")).toString());
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] const appellate::model::WorkflowEventHeader&
realismEventHeader(const appellate::model::WorkflowEvent& event) {
    return std::visit(
        [](const auto& concrete) -> const appellate::model::WorkflowEventHeader& {
            return concrete.header;
        },
        event);
}

[[nodiscard]] std::optional<QJsonObject>
executedFixtureTrace(const appellate::packs::RuntimePack& runtime) {
    if (runtime.cases.empty()) {
        return std::nullopt;
    }
    const auto& runtime_case = runtime.cases.front();
    const auto court_date = appellate::model::LegalDate{
        std::chrono::year{2026} / std::chrono::month{1} / std::chrono::day{4}};
    const appellate::model::WorkflowState initial_state{
        "example.session.realism-evidence",
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
    const appellate::model::WorkflowCommand command = appellate::model::SubmitWorkflowFiling{
        appellate::model::WorkflowCommandHeader{
            initial_state.session_id,
            appellate::model::WorkflowCommandId{"example.command.realism-notice"},
            appellate::model::ActorId{"example.actor.appellant"},
            appellate::model::LegalTime{
                std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}}, court_date},
        },
        appellate::model::WorkflowFilingId{"example.filing.realism-notice"},
        appellate::model::FilingTypeId{"example.filing.notice"},
        std::string(64, 'a'),
        {{appellate::model::FilingFieldId{"example.field.caption"}, "Example caption"}},
        {appellate::model::ActorId{"example.actor.appellee"}},
        std::nullopt,
    };
    const auto events = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, command);
    if (!events || events->empty()) {
        return std::nullopt;
    }
    const std::vector journal{
        appellate::model::WorkflowJournalEntry{command, *events},
    };
    const auto replayed = appellate::engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, journal);
    const auto command_bytes = appellate::storage::encodeWorkflowCommand(command);
    if (!replayed || !command_bytes) {
        return std::nullopt;
    }

    QCryptographicHash journal_hash(QCryptographicHash::Sha256);
    addFrame(journal_hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(journal_hash, journal.size());
    addFrame(journal_hash, QByteArrayView(*command_bytes));
    addUint64(journal_hash, events->size());

    QJsonArray encoded_events;
    QJsonArray operation_ids;
    for (const auto& event : *events) {
        const auto event_bytes = appellate::storage::encodeWorkflowEvent(event);
        if (!event_bytes) {
            return std::nullopt;
        }
        addFrame(journal_hash, QByteArrayView(*event_bytes));
        encoded_events.push_back(QString::fromLatin1(event_bytes->toBase64()));
        operation_ids.push_back(
            QString::fromStdString(realismEventHeader(event).operation_id.value));
    }
    const QJsonArray encoded_journal{QJsonObject{
        {QStringLiteral("command_base64"), QString::fromLatin1(command_bytes->toBase64())},
        {QStringLiteral("events_base64"), encoded_events},
    }};
    QJsonObject trace{
        {QStringLiteral("evidence_id"), QStringLiteral("example.evidence.trace-1")},
        {QStringLiteral("trace_id"), QStringLiteral("example.trace.canonical")},
        {QStringLiteral("workflow_id"), QString::fromStdString(runtime_case.workflow.id.value)},
        {QStringLiteral("engine_revision"), QStringLiteral("engine.example.realism.v1")},
        {QStringLiteral("command_count"), static_cast<qint64>(journal.size())},
        {QStringLiteral("event_count"), static_cast<qint64>(events->size())},
        {QStringLiteral("journal_sha256"), QString::fromLatin1(journal_hash.result().toHex())},
        {QStringLiteral("journal"), encoded_journal},
        {QStringLiteral("operation_ids"), operation_ids},
        {QStringLiteral("terminal_stage_id"),
         QString::fromStdString(replayed->current_stage_id.value)},
    };
    trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(QString::fromStdString(runtime_case.definition.id.value), trace));
    return trace;
}

[[nodiscard]] std::optional<QJsonObject>
executedFixtureTrace(const appellate::packs::LoadedPack& case_owner) {
    const auto runtime = appellate::packs::loadRuntimePack(case_owner);
    return runtime ? executedFixtureTrace(*runtime) : std::nullopt;
}

[[nodiscard]] std::optional<QJsonObject>
replayFirstTraceCommand(const QJsonObject& source_trace,
                        const appellate::packs::RuntimeCase& runtime_case, const QString& case_id) {
    const auto source_journal = source_trace.value(QStringLiteral("journal")).toArray();
    if (source_journal.isEmpty()) {
        return std::nullopt;
    }
    const auto source_entry = source_journal.first().toObject();
    const auto command_bytes = QByteArray::fromBase64(
        source_entry.value(QStringLiteral("command_base64")).toString().toLatin1());
    const auto command = appellate::storage::decodeWorkflowCommand(QByteArrayView(command_bytes));
    if (!command) {
        return std::nullopt;
    }
    const auto& header = std::visit(
        [](const auto& concrete) -> const appellate::model::WorkflowCommandHeader& {
            return concrete.header;
        },
        *command);
    const appellate::model::WorkflowState initial_state{
        header.session_id,
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
    const auto events = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, *command);
    if (!events || events->empty()) {
        return std::nullopt;
    }
    const std::vector journal{appellate::model::WorkflowJournalEntry{*command, *events}};
    const auto replayed = appellate::engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, journal);
    if (!replayed) {
        return std::nullopt;
    }

    QCryptographicHash journal_hash(QCryptographicHash::Sha256);
    addFrame(journal_hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(journal_hash, 1);
    addFrame(journal_hash, QByteArrayView(command_bytes));
    addUint64(journal_hash, events->size());
    QJsonArray encoded_events;
    QJsonArray operation_ids;
    for (const auto& event : *events) {
        const auto event_bytes = appellate::storage::encodeWorkflowEvent(event);
        if (!event_bytes) {
            return std::nullopt;
        }
        addFrame(journal_hash, QByteArrayView(*event_bytes));
        encoded_events.push_back(QString::fromLatin1(event_bytes->toBase64()));
        operation_ids.push_back(
            QString::fromStdString(realismEventHeader(event).operation_id.value));
    }
    QJsonObject trace{
        {QStringLiteral("evidence_id"), QStringLiteral("test.detached.evidence.different-trace")},
        {QStringLiteral("trace_id"), QStringLiteral("test.detached.trace.different-case")},
        {QStringLiteral("workflow_id"), QString::fromStdString(runtime_case.workflow.id.value)},
        {QStringLiteral("engine_revision"), QStringLiteral("test.manual.realism-profile")},
        {QStringLiteral("command_count"), 1},
        {QStringLiteral("event_count"), static_cast<qint64>(events->size())},
        {QStringLiteral("journal_sha256"), QString::fromLatin1(journal_hash.result().toHex())},
        {QStringLiteral("journal"),
         QJsonArray{QJsonObject{
             {QStringLiteral("command_base64"), QString::fromLatin1(command_bytes.toBase64())},
             {QStringLiteral("events_base64"), encoded_events},
         }}},
        {QStringLiteral("operation_ids"), operation_ids},
        {QStringLiteral("terminal_stage_id"),
         QString::fromStdString(replayed->current_stage_id.value)},
    };
    trace.insert(QStringLiteral("digest"), realismTraceDigest(case_id, trace));
    return trace;
}

[[nodiscard]] std::optional<QString> realismJournalDigest(const QJsonArray& journal) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-executed-workflow-journal-v1"));
    addUint64(hash, static_cast<std::uint64_t>(journal.size()));
    for (const auto& value : journal) {
        const auto entry = value.toObject();
        const auto command_encoded =
            entry.value(QStringLiteral("command_base64")).toString().toLatin1();
        const auto command = QByteArray::fromBase64(command_encoded);
        if (command.isEmpty() || command.toBase64() != command_encoded) {
            return std::nullopt;
        }
        addFrame(hash, QByteArrayView(command));
        const auto events = entry.value(QStringLiteral("events_base64")).toArray();
        addUint64(hash, static_cast<std::uint64_t>(events.size()));
        for (const auto& event_value : events) {
            const auto event_encoded = event_value.toString().toLatin1();
            const auto event = QByteArray::fromBase64(event_encoded);
            if (event.isEmpty() || event.toBase64() != event_encoded) {
                return std::nullopt;
            }
            addFrame(hash, QByteArrayView(event));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString realismRecordCheckDigest(const QString& case_id, const QJsonObject& check,
                                               const QJsonObject& record, QJsonArray blob_values) {
    const auto blobs = objects(blob_values, realismBlobLess);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addFrame(hash, QStringLiteral("appellate-workbench-record-check-evidence-v1"));
    addFrame(hash, case_id);
    addFrame(hash, check.value(QStringLiteral("evidence_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_id")).toString());
    addFrame(hash, check.value(QStringLiteral("record_id")).toString());
    addFrame(hash, check.value(QStringLiteral("check_kind")).toString());
    addRealismResourceDescriptor(hash, record);
    addUint64(hash, blobs.size());
    for (const auto& blob : blobs) {
        addRealismBlobDescriptor(hash, blob);
    }
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QString resourceKindName(appellate::model::ResourceKind kind) {
    switch (kind) {
    case appellate::model::ResourceKind::ArgumentConfig:
        return QStringLiteral("argument_config");
    case appellate::model::ResourceKind::AuthoritySet:
        return QStringLiteral("authority_set");
    case appellate::model::ResourceKind::BenchConfiguration:
        return QStringLiteral("bench_configuration");
    case appellate::model::ResourceKind::Case:
        return QStringLiteral("case");
    case appellate::model::ResourceKind::Court:
        return QStringLiteral("court");
    case appellate::model::ResourceKind::FilingCatalog:
        return QStringLiteral("filing_catalog");
    case appellate::model::ResourceKind::Form:
        return QStringLiteral("form");
    case appellate::model::ResourceKind::JudgeProfile:
        return QStringLiteral("judge_profile");
    case appellate::model::ResourceKind::ProcedureProfile:
        return QStringLiteral("procedure_profile");
    case appellate::model::ResourceKind::RealismReview:
        return QStringLiteral("realism_review");
    case appellate::model::ResourceKind::Record:
        return QStringLiteral("record");
    case appellate::model::ResourceKind::Workflow:
        return QStringLiteral("workflow");
    }
    return {};
}

[[nodiscard]] QString testPackRevisionDigest(const appellate::packs::LoadedPack& pack) {
    auto capabilities = pack.required_capabilities;
    auto dependencies = pack.dependencies;
    std::vector<const appellate::packs::ValidatedResource*> resources;
    resources.reserve(pack.resources.size());
    for (const auto& resource : pack.resources) {
        resources.push_back(&resource);
    }
    auto blobs = pack.blobs;
    std::ranges::sort(capabilities, {}, &appellate::model::RequiredCapability::id);
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
        addFrame(hash, resourceKindName(resource->descriptor.kind));
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

[[nodiscard]] QByteArray serializedReviewObject(const QJsonObject& object) {
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!bytes.endsWith('\n')) {
        bytes.push_back('\n');
    }
    return bytes;
}

[[nodiscard]] QString sha256Bytes(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void refreshResourceDigest(appellate::packs::ValidatedResource& resource) {
    resource.descriptor.sha256 =
        sha256Bytes(serializedReviewObject(resource.document)).toLatin1().toStdString();
}

void refreshPackRevision(appellate::packs::LoadedPack& pack) {
    pack.revision.digest = testPackRevisionDigest(pack).toLatin1().toStdString();
}

[[nodiscard]] QJsonObject realismPackBinding(const appellate::packs::LoadedPack& pack) {
    QJsonArray capabilities;
    for (const auto& capability : pack.required_capabilities) {
        capabilities.push_back(QJsonObject{
            {QStringLiteral("id"), QString::fromStdString(capability.id)},
            {QStringLiteral("version"), static_cast<qint64>(capability.version)},
        });
    }
    QJsonArray dependencies;
    for (const auto& dependency_value : pack.dependencies) {
        dependencies.push_back(QJsonObject{
            {QStringLiteral("pack_id"), QString::fromStdString(dependency_value.revision.id.value)},
            {QStringLiteral("version"), QString::fromStdString(dependency_value.revision.version)},
        });
    }
    return QJsonObject{
        {QStringLiteral("pack_id"), QString::fromStdString(pack.revision.id.value)},
        {QStringLiteral("version"), QString::fromStdString(pack.revision.version)},
        {QStringLiteral("manifest_schema_version"),
         static_cast<qint64>(pack.manifest_schema_version)},
        {QStringLiteral("required_capabilities"), capabilities},
        {QStringLiteral("dependencies"), dependencies},
    };
}

[[nodiscard]] std::optional<QJsonObject>
buildRealismReview(const appellate::packs::ValidatedResource& source_review,
                   const appellate::packs::LoadedPack& case_owner,
                   std::span<const appellate::packs::LoadedPack* const> subject_packs,
                   bool independently_reviewed) {
    QHash<QString, const appellate::packs::ValidatedResource*> resources_by_id;
    QHash<QString, const appellate::packs::LoadedPack*> owners_by_resource_id;
    QHash<QString, QJsonObject> resource_bindings_by_id;
    QHash<QString, QJsonObject> blob_bindings_by_owner_path;
    QJsonArray pack_bindings;
    QJsonArray resource_bindings;
    QJsonArray blob_bindings;
    int resource_index = 0;
    int blob_index = 0;
    for (const auto* pack : subject_packs) {
        if (pack == nullptr) {
            return std::nullopt;
        }
        pack_bindings.push_back(realismPackBinding(*pack));
        for (const auto& resource : pack->resources) {
            resources_by_id.insert(QString::fromStdString(resource.descriptor.id), &resource);
            owners_by_resource_id.insert(QString::fromStdString(resource.descriptor.id), pack);
            if (resource.descriptor.kind == appellate::model::ResourceKind::RealismReview) {
                continue;
            }
            ++resource_index;
            const auto binding = QJsonObject{
                {QStringLiteral("evidence_id"),
                 QStringLiteral("example.evidence.resource-%1").arg(resource_index)},
                {QStringLiteral("owner_pack_id"), QString::fromStdString(pack->revision.id.value)},
                {QStringLiteral("owner_pack_version"),
                 QString::fromStdString(pack->revision.version)},
                {QStringLiteral("resource_id"), QString::fromStdString(resource.descriptor.id)},
                {QStringLiteral("resource_kind"), resourceKindName(resource.descriptor.kind)},
                {QStringLiteral("schema_version"),
                 static_cast<qint64>(resource.descriptor.schema_version)},
                {QStringLiteral("path"), QString::fromStdString(resource.descriptor.path)},
                {QStringLiteral("sha256"), QString::fromStdString(resource.descriptor.sha256)},
            };
            resource_bindings.push_back(binding);
            resource_bindings_by_id.insert(QString::fromStdString(resource.descriptor.id), binding);
        }
        for (const auto& blob : pack->blobs) {
            ++blob_index;
            const auto binding = QJsonObject{
                {QStringLiteral("evidence_id"),
                 QStringLiteral("example.evidence.blob-%1").arg(blob_index)},
                {QStringLiteral("owner_pack_id"), QString::fromStdString(pack->revision.id.value)},
                {QStringLiteral("owner_pack_version"),
                 QString::fromStdString(pack->revision.version)},
                {QStringLiteral("path"), QString::fromStdString(blob.path)},
                {QStringLiteral("media_type"), QString::fromStdString(blob.media_type)},
                {QStringLiteral("byte_size"), static_cast<qint64>(blob.byte_size)},
                {QStringLiteral("sha256"), QString::fromStdString(blob.sha256)},
            };
            blob_bindings.push_back(binding);
            blob_bindings_by_owner_path.insert(
                QString::fromStdString(pack->revision.id.value + '\n' + blob.path), binding);
        }
    }

    const auto case_id = source_review.document.value(QStringLiteral("case_id")).toString();
    const auto case_resource = resources_by_id.value(case_id);
    if (case_resource == nullptr || owners_by_resource_id.value(case_id) != &case_owner) {
        return std::nullopt;
    }
    const auto procedure_id =
        case_resource->document.value(QStringLiteral("procedure_profile_id")).toString();
    const auto procedure = resources_by_id.value(procedure_id);
    if (procedure == nullptr) {
        return std::nullopt;
    }
    const auto workflow_id = procedure->document.value(QStringLiteral("workflow_id")).toString();
    const auto workflow = resources_by_id.value(workflow_id);
    const auto record_id = case_resource->document.value(QStringLiteral("record_id")).toString();
    const auto record = resources_by_id.value(record_id);
    const auto record_owner = owners_by_resource_id.value(record_id);
    if (workflow == nullptr || record == nullptr || record_owner == nullptr) {
        return std::nullopt;
    }

    const auto prior_traces = source_review.document.value(QStringLiteral("evidence"))
                                  .toObject()
                                  .value(QStringLiteral("traces"))
                                  .toArray();
    auto trace = prior_traces.isEmpty() ? QJsonObject{} : prior_traces.first().toObject();
    if (!trace.contains(QStringLiteral("journal"))) {
        const auto executed = executedFixtureTrace(case_owner);
        if (!executed) {
            return std::nullopt;
        }
        trace = *executed;
    }

    QJsonArray record_blobs;
    for (const auto& value : record->document.value(QStringLiteral("docket_entries")).toArray()) {
        const auto path = value.toObject().value(QStringLiteral("asset_path")).toString();
        const auto binding = blob_bindings_by_owner_path.value(
            QString::fromStdString(record_owner->revision.id.value) + u'\n' + path);
        if (binding.isEmpty()) {
            return std::nullopt;
        }
        record_blobs.push_back(binding);
    }
    const auto record_binding = resource_bindings_by_id.value(record_id);
    if (record_binding.isEmpty()) {
        return std::nullopt;
    }
    QJsonObject asset_check{
        {QStringLiteral("evidence_id"), QStringLiteral("example.evidence.record-assets")},
        {QStringLiteral("check_id"), QStringLiteral("example.check.record-assets")},
        {QStringLiteral("record_id"), record_id},
        {QStringLiteral("check_kind"), QStringLiteral("asset_resolution")},
    };
    asset_check.insert(
        QStringLiteral("digest"),
        realismRecordCheckDigest(case_id, asset_check, record_binding, record_blobs));
    QJsonObject anchor_check{
        {QStringLiteral("evidence_id"), QStringLiteral("example.evidence.record-anchors")},
        {QStringLiteral("check_id"), QStringLiteral("example.check.record-anchors")},
        {QStringLiteral("record_id"), record_id},
        {QStringLiteral("check_kind"), QStringLiteral("page_anchor_resolution")},
    };
    anchor_check.insert(QStringLiteral("digest"),
                        realismRecordCheckDigest(case_id, anchor_check, record_binding, {}));

    QJsonArray authority_bindings;
    for (const auto* pack : subject_packs) {
        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind != appellate::model::ResourceKind::AuthoritySet) {
                continue;
            }
            const auto authorities =
                resource.document.value(QStringLiteral("authorities")).toArray();
            if (!authorities.isEmpty()) {
                authority_bindings.push_back(QJsonObject{
                    {QStringLiteral("evidence_id"), QStringLiteral("example.evidence.authority-1")},
                    {QStringLiteral("authority_id"),
                     authorities.first().toObject().value(QStringLiteral("authority_id"))},
                });
            }
        }
    }
    if (authority_bindings.isEmpty() || resource_bindings.isEmpty() || blob_bindings.isEmpty()) {
        return std::nullopt;
    }

    const auto resource_evidence_id =
        resource_bindings.first().toObject().value(QStringLiteral("evidence_id"));
    const auto authority_evidence_id =
        authority_bindings.first().toObject().value(QStringLiteral("evidence_id"));
    const QJsonObject dimension_evidence{
        {QStringLiteral("procedural_law"), QJsonArray{QStringLiteral("example.evidence.trace-1")}},
        {QStringLiteral("deadlines_authority"), QJsonArray{authority_evidence_id}},
        {QStringLiteral("record_consistency"),
         QJsonArray{QStringLiteral("example.evidence.record-assets"),
                    QStringLiteral("example.evidence.record-anchors")}},
        {QStringLiteral("consequences"), QJsonArray{QStringLiteral("example.evidence.trace-1")}},
        {QStringLiteral("oral_argument"), QJsonArray{resource_evidence_id}},
        {QStringLiteral("bench_differentiation"), QJsonArray{resource_evidence_id}},
        {QStringLiteral("provenance"), QJsonArray{authority_evidence_id}},
    };
    QJsonObject evidence{
        {QStringLiteral("packs"), pack_bindings},
        {QStringLiteral("resources"), resource_bindings},
        {QStringLiteral("blobs"), blob_bindings},
        {QStringLiteral("traces"), QJsonArray{trace}},
        {QStringLiteral("record_checks"), QJsonArray{asset_check, anchor_check}},
        {QStringLiteral("authorities"), authority_bindings},
        {QStringLiteral("dimension_evidence"), dimension_evidence},
    };
    evidence.insert(QStringLiteral("closure_digest"),
                    realismClosureDigest(case_id, pack_bindings, resource_bindings, blob_bindings));

    auto review = source_review.document;
    review.insert(QStringLiteral("review_state"), independently_reviewed
                                                      ? QStringLiteral("independently_reviewed")
                                                      : QStringLiteral("self_reviewed"));
    review.insert(QStringLiteral("dimensions"),
                  QJsonObject{
                      {QStringLiteral("procedural_law"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("deadlines_authority"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("record_consistency"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("consequences"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("oral_argument"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("bench_differentiation"), independently_reviewed ? 3 : 2},
                      {QStringLiteral("provenance"), independently_reviewed ? 3 : 2},
                  });
    review.insert(QStringLiteral("known_uncertainty"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("uncertainty_id"),
                       QStringLiteral("example.uncertainty.fictional-authorities")},
                      {QStringLiteral("summary"),
                       QStringLiteral("The authorities are deliberately fictional test fixtures.")},
                      {QStringLiteral("blocking"), false},
                  }});
    review.insert(QStringLiteral("evidence"), evidence);
    if (independently_reviewed) {
        review.insert(QStringLiteral("reviewed_on"), QStringLiteral("2026-08-11"));
        review.insert(QStringLiteral("reviewer_reference"),
                      QStringLiteral("Independent fixture review memorandum"));
        review.insert(
            QStringLiteral("reviewer"),
            QJsonObject{
                {QStringLiteral("reviewer_id"), QStringLiteral("example.reviewer.independent")},
                {QStringLiteral("display_name"), QStringLiteral("Independent Reviewer")},
                {QStringLiteral("qualification"),
                 QStringLiteral("Qualified appellate practitioner")},
                {QStringLiteral("affiliation"), QStringLiteral("Independent fixture review")},
            });
    } else {
        review.remove(QStringLiteral("reviewed_on"));
        review.remove(QStringLiteral("reviewer_reference"));
        review.remove(QStringLiteral("reviewer"));
    }
    return review;
}

[[nodiscard]] bool prepareRealismEvidencePack(const QString& root) {
    auto staged = PackReader::readDirectory(root);
    if (!staged) {
        return false;
    }
    if (!mutateManifest(root, [](QJsonObject& manifest) {
            auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
            capabilities.push_back(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("workbench.pack.realism-evidence")},
                {QStringLiteral("version"), 1},
            });
            manifest.insert(QStringLiteral("required_capabilities"), capabilities);
        })) {
        return false;
    }
    const auto case_resource = std::ranges::find_if(staged->resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Case;
    });
    if (case_resource == staged->resources.end()) {
        return false;
    }
    const auto review_path = QStringLiteral("resources/realism-review.json");
    const appellate::packs::ValidatedResource review{
        appellate::model::DeclarativeResource{
            appellate::model::ResourceKind::RealismReview,
            "example.review.fictional",
            2,
            review_path.toStdString(),
            {},
        },
        QJsonObject{
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
            {QStringLiteral("resource_id"), QStringLiteral("example.review.fictional")},
            {QStringLiteral("case_id"), QString::fromStdString(case_resource->descriptor.id)},
            {QStringLiteral("review_state"), QStringLiteral("self_reviewed")},
            {QStringLiteral("dimensions"),
             QJsonObject{
                 {QStringLiteral("procedural_law"), 2},
                 {QStringLiteral("deadlines_authority"), 2},
                 {QStringLiteral("record_consistency"), 2},
                 {QStringLiteral("consequences"), 2},
                 {QStringLiteral("oral_argument"), 2},
                 {QStringLiteral("bench_differentiation"), 2},
                 {QStringLiteral("provenance"), 2},
             }},
            {QStringLiteral("known_uncertainty"), QJsonArray{}},
        },
    };
    const std::array<const appellate::packs::LoadedPack*, 1> subject_packs{&*staged};
    auto document = buildRealismReview(review, *staged, subject_packs, false);
    if (!document) {
        return false;
    }
    auto evidence = document->value(QStringLiteral("evidence")).toObject();
    auto packs = evidence.value(QStringLiteral("packs")).toArray();
    auto pack = packs.first().toObject();
    auto capabilities = pack.value(QStringLiteral("required_capabilities")).toArray();
    capabilities.push_back(QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.realism-evidence")},
        {QStringLiteral("version"), 1},
    });
    pack.insert(QStringLiteral("required_capabilities"), capabilities);
    packs.replace(0, pack);
    evidence.insert(QStringLiteral("packs"), packs);
    evidence.insert(QStringLiteral("closure_digest"),
                    realismClosureDigest(document->value(QStringLiteral("case_id")).toString(),
                                         packs,
                                         evidence.value(QStringLiteral("resources")).toArray(),
                                         evidence.value(QStringLiteral("blobs")).toArray()));
    document->insert(QStringLiteral("evidence"), evidence);
    const auto bytes = QJsonDocument(*document).toJson(QJsonDocument::Compact);
    if (!writeBytes(QDir(root).filePath(review_path), bytes)) {
        return false;
    }
    const auto digest =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return mutateManifest(root, [&](QJsonObject& manifest) {
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        contents.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("example.review.fictional")},
            {QStringLiteral("kind"), QStringLiteral("realism_review")},
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("path"), review_path},
            {QStringLiteral("sha256"), digest},
        });
        manifest.insert(QStringLiteral("contents"), contents);
    });
}

struct StrictDetachedFixture final {
    appellate::packs::LoadedPack federal;
    appellate::packs::LoadedPack ca4;
    appellate::packs::LoadedPack bench;
    appellate::packs::LoadedPack subject;
    appellate::packs::LoadedPack detached;
};

[[nodiscard]] QString contentPath(const QString& relative_path) {
    const auto content_root =
        QDir::cleanPath(QStringLiteral(APPELLATE_GOLD_PACK) + QStringLiteral("/../.."));
    return QDir(content_root).filePath(relative_path);
}

[[nodiscard]] std::optional<StrictDetachedFixture> strictDetachedFixture() {
    auto federal =
        PackReader::readDirectory(contentPath(QStringLiteral("foundations/us-federal/pack")));
    auto ca4 = PackReader::readDirectory(contentPath(QStringLiteral("foundations/us-ca4/pack")),
                                         appellate::packs::PackValidationScope::ResolvedClosure);
    auto bench = PackReader::readDirectory(
        contentPath(QStringLiteral("foundations/us-ca4-fictional-bench/pack")));
    auto subject =
        PackReader::readDirectory(contentPath(QStringLiteral("m4/cinderlake-writ/pack-candidate")),
                                  appellate::packs::PackValidationScope::ResolvedClosure);
    if (!federal || !ca4 || !bench || !subject) {
        return std::nullopt;
    }
    const std::array<const appellate::packs::LoadedPack*, 1> ca4_dependencies{&*federal};
    if (!PackReader::validateResolvedGraph(*ca4, ca4_dependencies)) {
        return std::nullopt;
    }
    const std::array<const appellate::packs::LoadedPack*, 3> subject_dependencies{&*federal, &*ca4,
                                                                                  &*bench};
    if (!PackReader::validateResolvedGraph(*subject, subject_dependencies)) {
        return std::nullopt;
    }

    const auto source_review = std::ranges::find_if(subject->resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    if (source_review == subject->resources.end()) {
        return std::nullopt;
    }
    auto document = source_review->document;
    const auto detached_resource_id = QStringLiteral("test.detached.review");
    document.insert(QStringLiteral("resource_id"), detached_resource_id);
    document.insert(QStringLiteral("review_state"), QStringLiteral("independently_reviewed"));
    document.insert(QStringLiteral("reviewed_on"), QStringLiteral("2099-12-31"));
    document.insert(QStringLiteral("reviewer_reference"),
                    QStringLiteral("TEST-ONLY independent review declaration"));
    document.insert(
        QStringLiteral("reviewer"),
        QJsonObject{
            {QStringLiteral("reviewer_id"), QStringLiteral("test.detached.reviewer")},
            {QStringLiteral("display_name"), QStringLiteral("TEST-ONLY Reviewer")},
            {QStringLiteral("qualification"),
             QStringLiteral("TEST-ONLY fixture; no human independent review was performed")},
        });
    auto dimensions = document.value(QStringLiteral("dimensions")).toObject();
    for (const auto* name :
         {"procedural_law", "deadlines_authority", "record_consistency", "consequences",
          "oral_argument", "bench_differentiation", "provenance"}) {
        dimensions.insert(QLatin1StringView(name), 3);
    }
    document.insert(QStringLiteral("dimensions"), dimensions);
    auto evidence = document.value(QStringLiteral("evidence")).toObject();
    auto traces = evidence.value(QStringLiteral("traces")).toArray();
    const auto case_id = document.value(QStringLiteral("case_id")).toString();
    for (qsizetype index = 0; index < traces.size(); ++index) {
        auto trace = traces.at(index).toObject();
        trace.insert(QStringLiteral("engine_revision"),
                     QStringLiteral("appellate.realism-evidence.detached-review-replay.v1"));
        trace.insert(QStringLiteral("digest"), realismTraceDigest(case_id, trace));
        traces.replace(index, trace);
    }
    evidence.insert(QStringLiteral("traces"), traces);
    document.insert(QStringLiteral("evidence"), evidence);

    auto descriptor = source_review->descriptor;
    descriptor.id = detached_resource_id.toStdString();
    descriptor.path = "resources/realism-review.json";
    descriptor.sha256 = sha256Bytes(serializedReviewObject(document)).toLatin1().toStdString();
    appellate::packs::LoadedPack detached{
        2,
        appellate::model::PackRevision{appellate::model::PackId{"test.detached.pack"}, "1.0.0", {}},
        {
            appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2},
            appellate::model::RequiredCapability{"workbench.pack.realism-evidence", 1},
        },
        {appellate::model::PackDependency{subject->revision}},
        {appellate::packs::ValidatedResource{std::move(descriptor), std::move(document)}},
        {},
        {},
        appellate::packs::PackGraphState::DeferredReferences,
    };
    refreshPackRevision(detached);
    return StrictDetachedFixture{std::move(*federal), std::move(*ca4), std::move(*bench),
                                 std::move(*subject), std::move(detached)};
}

[[nodiscard]] std::expected<void, appellate::packs::Error> validateStrictDetached(
    const StrictDetachedFixture& fixture,
    const appellate::packs::LoadedPack* subject_override = nullptr,
    const appellate::packs::LoadedPack* detached_override = nullptr,
    std::span<const appellate::packs::LoadedPack* const> additional_subject_packs = {}) {
    const auto* subject = subject_override == nullptr ? &fixture.subject : subject_override;
    const auto* detached = detached_override == nullptr ? &fixture.detached : detached_override;
    std::optional<appellate::packs::LoadedPack> repaired_detached;
    if (detached->resources.size() == 1 &&
        detached->resources.front().descriptor.sha256 != std::string(64, '0')) {
        repaired_detached = *detached;
        refreshResourceDigest(repaired_detached->resources.front());
        refreshPackRevision(*repaired_detached);
        detached = &*repaired_detached;
    }
    std::vector<const appellate::packs::LoadedPack*> closure{&fixture.federal, &fixture.ca4,
                                                             &fixture.bench};
    closure.insert(closure.end(), additional_subject_packs.begin(), additional_subject_packs.end());
    closure.push_back(subject);
    return PackReader::validateResolvedGraph(*detached, closure);
}

[[nodiscard]] appellate::packs::ValidatedResource*
firstRealismReview(appellate::packs::LoadedPack& pack) {
    const auto found = std::ranges::find_if(pack.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    return found == pack.resources.end() ? nullptr : &*found;
}

[[nodiscard]] bool setReviewTraceProfile(appellate::packs::ValidatedResource& review,
                                         const QString& engine_revision, bool keep_first_only) {
    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    auto traces = evidence.value(QStringLiteral("traces")).toArray();
    if (traces.isEmpty()) {
        return false;
    }
    QSet<QString> removed_trace_refs;
    if (keep_first_only) {
        for (qsizetype index = 1; index < traces.size(); ++index) {
            removed_trace_refs.insert(
                traces.at(index).toObject().value(QStringLiteral("evidence_id")).toString());
        }
        traces = QJsonArray{traces.first()};
    }
    const auto case_id = review.document.value(QStringLiteral("case_id")).toString();
    for (qsizetype index = 0; index < traces.size(); ++index) {
        auto trace = traces.at(index).toObject();
        trace.insert(QStringLiteral("engine_revision"), engine_revision);
        trace.insert(QStringLiteral("digest"), realismTraceDigest(case_id, trace));
        traces.replace(index, trace);
    }
    evidence.insert(QStringLiteral("traces"), traces);
    if (keep_first_only) {
        auto groups = evidence.value(QStringLiteral("dimension_evidence")).toObject();
        for (const auto& key : groups.keys()) {
            QJsonArray filtered;
            for (const auto& value : groups.value(key).toArray()) {
                if (!removed_trace_refs.contains(value.toString())) {
                    filtered.push_back(value);
                }
            }
            groups.insert(key, filtered);
        }
        evidence.insert(QStringLiteral("dimension_evidence"), groups);
        auto dimensions = review.document.value(QStringLiteral("dimensions")).toObject();
        for (const auto& key : dimensions.keys()) {
            dimensions.insert(key, 1);
        }
        review.document.insert(QStringLiteral("dimensions"), dimensions);
    }
    review.document.insert(QStringLiteral("evidence"), evidence);
    return true;
}

[[nodiscard]] bool mutateFirstTraceCommandField(appellate::packs::ValidatedResource& review,
                                                const QString& field_value) {
    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    auto traces = evidence.value(QStringLiteral("traces")).toArray();
    if (traces.isEmpty()) {
        return false;
    }
    auto trace = traces.first().toObject();
    auto journal = trace.value(QStringLiteral("journal")).toArray();
    if (journal.isEmpty()) {
        return false;
    }
    auto entry = journal.first().toObject();
    const auto command_bytes =
        QByteArray::fromBase64(entry.value(QStringLiteral("command_base64")).toString().toLatin1());
    auto command = QJsonDocument::fromJson(command_bytes).object();
    auto payload = command.value(QStringLiteral("payload")).toObject();
    auto fields = payload.value(QStringLiteral("fields")).toArray();
    if (fields.isEmpty()) {
        return false;
    }
    auto field = fields.first().toObject();
    field.insert(QStringLiteral("value"), field_value);
    fields.replace(0, field);
    payload.insert(QStringLiteral("fields"), fields);
    command.insert(QStringLiteral("payload"), payload);
    entry.insert(
        QStringLiteral("command_base64"),
        QString::fromLatin1(QJsonDocument(command).toJson(QJsonDocument::Compact).toBase64()));
    journal.replace(0, entry);
    const auto journal_digest = realismJournalDigest(journal);
    if (!journal_digest) {
        return false;
    }
    trace.insert(QStringLiteral("journal"), journal);
    trace.insert(QStringLiteral("journal_sha256"), *journal_digest);
    trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(review.document.value(QStringLiteral("case_id")).toString(), trace));
    traces.replace(0, trace);
    evidence.insert(QStringLiteral("traces"), traces);
    review.document.insert(QStringLiteral("evidence"), evidence);
    return true;
}

[[nodiscard]] bool mutateFirstTraceEventProposition(appellate::packs::ValidatedResource& review,
                                                    const QString& proposition) {
    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    auto traces = evidence.value(QStringLiteral("traces")).toArray();
    if (traces.isEmpty()) {
        return false;
    }
    auto trace = traces.first().toObject();
    auto journal = trace.value(QStringLiteral("journal")).toArray();
    if (journal.isEmpty()) {
        return false;
    }
    auto entry = journal.first().toObject();
    auto encoded_events = entry.value(QStringLiteral("events_base64")).toArray();
    if (encoded_events.isEmpty()) {
        return false;
    }
    const auto event_bytes = QByteArray::fromBase64(encoded_events.first().toString().toLatin1());
    auto event = QJsonDocument::fromJson(event_bytes).object();
    auto payload = event.value(QStringLiteral("payload")).toObject();
    auto authority = payload.value(QStringLiteral("authority")).toObject();
    auto primary = authority.value(QStringLiteral("primary")).toObject();
    if (!primary.contains(QStringLiteral("proposition"))) {
        return false;
    }
    primary.insert(QStringLiteral("proposition"), proposition);
    authority.insert(QStringLiteral("primary"), primary);
    payload.insert(QStringLiteral("authority"), authority);
    event.insert(QStringLiteral("payload"), payload);
    encoded_events.replace(
        0, QString::fromLatin1(QJsonDocument(event).toJson(QJsonDocument::Compact).toBase64()));
    entry.insert(QStringLiteral("events_base64"), encoded_events);
    journal.replace(0, entry);
    const auto journal_digest = realismJournalDigest(journal);
    if (!journal_digest) {
        return false;
    }
    trace.insert(QStringLiteral("journal"), journal);
    trace.insert(QStringLiteral("journal_sha256"), *journal_digest);
    trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(review.document.value(QStringLiteral("case_id")).toString(), trace));
    traces.replace(0, trace);
    evidence.insert(QStringLiteral("traces"), traces);
    review.document.insert(QStringLiteral("evidence"), evidence);
    return true;
}

void replayDetachedTracesFromSource(const appellate::packs::ValidatedResource& source,
                                    appellate::packs::ValidatedResource& detached) {
    const auto source_traces = source.document.value(QStringLiteral("evidence"))
                                   .toObject()
                                   .value(QStringLiteral("traces"))
                                   .toArray();
    QJsonArray detached_traces;
    const auto case_id = detached.document.value(QStringLiteral("case_id")).toString();
    for (const auto& value : source_traces) {
        auto trace = value.toObject();
        trace.insert(QStringLiteral("engine_revision"),
                     QStringLiteral("appellate.realism-evidence.detached-review-replay.v1"));
        trace.insert(QStringLiteral("digest"), realismTraceDigest(case_id, trace));
        detached_traces.push_back(trace);
    }
    auto evidence = detached.document.value(QStringLiteral("evidence")).toObject();
    evidence.insert(QStringLiteral("traces"), detached_traces);
    detached.document.insert(QStringLiteral("evidence"), evidence);
}

[[nodiscard]] QSet<QString> removeSubjectResources(
    appellate::packs::LoadedPack& subject,
    const std::function<bool(const appellate::packs::ValidatedResource&)>& should_remove) {
    QSet<QString> removed;
    std::erase_if(subject.resources, [&](const auto& resource) {
        if (!should_remove(resource)) {
            return false;
        }
        removed.insert(QString::fromStdString(resource.descriptor.id));
        return true;
    });
    return removed;
}

void repairReviewAfterRemovedResources(appellate::packs::ValidatedResource& review,
                                       const QSet<QString>& removed_resource_ids) {
    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    auto resources = evidence.value(QStringLiteral("resources")).toArray();
    QSet<QString> removed_evidence_ids;
    QJsonArray retained_resources;
    for (const auto& value : resources) {
        const auto binding = value.toObject();
        if (removed_resource_ids.contains(
                binding.value(QStringLiteral("resource_id")).toString())) {
            removed_evidence_ids.insert(binding.value(QStringLiteral("evidence_id")).toString());
        } else {
            retained_resources.push_back(binding);
        }
    }
    evidence.insert(QStringLiteral("resources"), retained_resources);

    auto groups = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    for (const auto& key : groups.keys()) {
        QJsonArray retained;
        for (const auto& value : groups.value(key).toArray()) {
            if (!removed_evidence_ids.contains(value.toString())) {
                retained.push_back(value);
            }
        }
        groups.insert(key, retained);
    }
    evidence.insert(QStringLiteral("dimension_evidence"), groups);
    evidence.insert(
        QStringLiteral("closure_digest"),
        realismClosureDigest(review.document.value(QStringLiteral("case_id")).toString(),
                             evidence.value(QStringLiteral("packs")).toArray(), retained_resources,
                             evidence.value(QStringLiteral("blobs")).toArray()));
    review.document.insert(QStringLiteral("evidence"), evidence);
}

[[nodiscard]] QString codeOwnedEvidenceId(const QString& category,
                                          std::initializer_list<QString> identity) {
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

struct TestRealismClosureBindings final {
    QJsonArray packs;
    QJsonArray resources;
    QJsonArray blobs;
};

[[nodiscard]] TestRealismClosureBindings
testRealismClosureBindings(std::span<const appellate::packs::LoadedPack* const> subject_packs) {
    TestRealismClosureBindings bindings;
    for (const auto* pack : subject_packs) {
        bindings.packs.push_back(realismPackBinding(*pack));
        for (const auto& resource : pack->resources) {
            if (resource.descriptor.kind == appellate::model::ResourceKind::RealismReview) {
                continue;
            }
            const auto owner_id = QString::fromStdString(pack->revision.id.value);
            const auto owner_version = QString::fromStdString(pack->revision.version);
            const auto resource_id = QString::fromStdString(resource.descriptor.id);
            const auto kind = resourceKindName(resource.descriptor.kind);
            const auto schema_version = QString::number(resource.descriptor.schema_version);
            const auto path = QString::fromStdString(resource.descriptor.path);
            const auto digest = QString::fromStdString(resource.descriptor.sha256);
            bindings.resources.push_back(QJsonObject{
                {QStringLiteral("evidence_id"),
                 codeOwnedEvidenceId(
                     QStringLiteral("resource"),
                     {owner_id, owner_version, resource_id, kind, schema_version, path, digest})},
                {QStringLiteral("owner_pack_id"), owner_id},
                {QStringLiteral("owner_pack_version"), owner_version},
                {QStringLiteral("resource_id"), resource_id},
                {QStringLiteral("resource_kind"), kind},
                {QStringLiteral("schema_version"),
                 static_cast<qint64>(resource.descriptor.schema_version)},
                {QStringLiteral("path"), path},
                {QStringLiteral("sha256"), digest},
            });
        }
        for (const auto& blob : pack->blobs) {
            const auto owner_id = QString::fromStdString(pack->revision.id.value);
            const auto owner_version = QString::fromStdString(pack->revision.version);
            const auto path = QString::fromStdString(blob.path);
            const auto media_type = QString::fromStdString(blob.media_type);
            const auto byte_size = QString::number(static_cast<qulonglong>(blob.byte_size));
            const auto digest = QString::fromStdString(blob.sha256);
            bindings.blobs.push_back(QJsonObject{
                {QStringLiteral("evidence_id"),
                 codeOwnedEvidenceId(QStringLiteral("blob"), {owner_id, owner_version, path,
                                                              media_type, byte_size, digest})},
                {QStringLiteral("owner_pack_id"), owner_id},
                {QStringLiteral("owner_pack_version"), owner_version},
                {QStringLiteral("path"), path},
                {QStringLiteral("media_type"), media_type},
                {QStringLiteral("byte_size"), static_cast<qint64>(blob.byte_size)},
                {QStringLiteral("sha256"), digest},
            });
        }
    }
    return bindings;
}

void applyRealismClosureBindings(appellate::packs::ValidatedResource& review,
                                 const TestRealismClosureBindings& bindings) {
    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    evidence.insert(QStringLiteral("packs"), bindings.packs);
    evidence.insert(QStringLiteral("resources"), bindings.resources);
    evidence.insert(QStringLiteral("blobs"), bindings.blobs);
    evidence.insert(
        QStringLiteral("closure_digest"),
        realismClosureDigest(review.document.value(QStringLiteral("case_id")).toString(),
                             bindings.packs, bindings.resources, bindings.blobs));
    review.document.insert(QStringLiteral("evidence"), evidence);
}

void setReviewDimension(appellate::packs::ValidatedResource& review, const QString& dimension,
                        int score, bool require_generic_reference = false) {
    auto dimensions = review.document.value(QStringLiteral("dimensions")).toObject();
    dimensions.insert(dimension, score);
    review.document.insert(QStringLiteral("dimensions"), dimensions);

    auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
    auto groups = evidence.value(QStringLiteral("dimension_evidence")).toObject();
    if (score == 0) {
        groups.insert(dimension, QJsonArray{});
    } else if (require_generic_reference && groups.value(dimension).toArray().isEmpty()) {
        const auto resources = evidence.value(QStringLiteral("resources")).toArray();
        const auto record = std::ranges::find_if(resources, [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("resource_kind")).toString() ==
                   QStringLiteral("record");
        });
        if (record != resources.end()) {
            groups.insert(dimension,
                          QJsonArray{record->toObject().value(QStringLiteral("evidence_id"))});
        }
    }
    evidence.insert(QStringLiteral("dimension_evidence"), groups);
    review.document.insert(QStringLiteral("evidence"), evidence);
}

void refreshStrictDetachedFixture(StrictDetachedFixture& fixture) {
    auto* source_review = firstRealismReview(fixture.subject);
    Q_ASSERT(source_review != nullptr);
    refreshResourceDigest(*source_review);
    refreshPackRevision(fixture.subject);
    fixture.detached.dependencies = {appellate::model::PackDependency{fixture.subject.revision}};
    refreshResourceDigest(fixture.detached.resources.front());
    refreshPackRevision(fixture.detached);
}

void rebuildStrictDetachedSubjectClosure(
    StrictDetachedFixture& fixture,
    std::span<const appellate::packs::LoadedPack* const> additional_subject_packs) {
    std::vector<const appellate::packs::LoadedPack*> subject_packs{&fixture.federal, &fixture.ca4,
                                                                   &fixture.bench};
    subject_packs.insert(subject_packs.end(), additional_subject_packs.begin(),
                         additional_subject_packs.end());
    subject_packs.push_back(&fixture.subject);
    const auto bindings = testRealismClosureBindings(subject_packs);
    auto* source = firstRealismReview(fixture.subject);
    Q_ASSERT(source != nullptr);
    applyRealismClosureBindings(*source, bindings);
    applyRealismClosureBindings(fixture.detached.resources.front(), bindings);
    refreshStrictDetachedFixture(fixture);
}

void useManualTraceProfile(appellate::packs::ValidatedResource& review) {
    static_cast<void>(
        setReviewTraceProfile(review, QStringLiteral("test.manual.realism-profile"), false));
}

void SchemaDispatchTest::preservesPinnedV1Digests() {
    const auto fixture_pack =
        PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    QVERIFY2(fixture_pack.has_value(),
             fixture_pack ? "" : qPrintable(fixture_pack.error().message));
    QCOMPARE(fixture_pack->manifest_schema_version, std::uint32_t{1});
    QCOMPARE(fixture_pack->revision.digest,
             std::string("b76d4e2f8285a3c250163dd3aae7bb223f03bbf0eca29fa91499ab2e5bd52038"));

    const auto gold = PackReader::readDirectory(QStringLiteral(APPELLATE_GOLD_PACK));
    QVERIFY2(gold.has_value(), gold ? "" : qPrintable(gold.error().message));
    QCOMPARE(gold->manifest_schema_version, std::uint32_t{1});
    QCOMPARE(gold->revision.digest,
             std::string("ff7a2e1195f9bd006e7df46c19675a3e07a4bd8975b1643a01adbc9cc4fd3424"));
}

void SchemaDispatchTest::preservesPinnedPre27V2Revision() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyPre27V2Fixture(pack.path()));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->revision.digest,
             std::string("a9c912ad7e23620f9a5c9f5fb81c9edabe1d00010551c4636e8a621b00655bd4"));
}

void SchemaDispatchTest::preservesPinnedPre28V2Revision() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-27-overlay")), pack.path()));
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-25-overlay")), pack.path()));
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-28-overlay")), pack.path()));
    QVERIFY(QFile::remove(
        QDir(pack.path())
            .filePath(QStringLiteral("resources/argument-config-counterfactual.json"))));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->manifest_schema_version, std::uint32_t{2});
    QCOMPARE(loaded->revision.digest,
             std::string("e36b712c5f845148a61b65992077119551c0521e39679b0f1572f76217882b54"));

    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    const auto& runtime_case = runtime->cases.front();
    QVERIFY(runtime_case.definition.disposition_targets.empty());
    QVERIFY(runtime_case.definition.disposition_plans.empty());
    QVERIFY(!runtime_case.definition.authored_disposition_plan_id.has_value());
    QVERIFY(!runtime_case.definition.authored_disposition_operation_id.has_value());
    QVERIFY(std::ranges::all_of(runtime_case.workflow.operations, [](const auto& operation) {
        return operation.preconditions.empty();
    }));
    QCOMPARE(runtime_case.argument_configurations.size(), std::size_t{1});
    QVERIFY(!runtime_case.argument_configurations.front().grounded_question_bank.has_value());
}

void SchemaDispatchTest::preservesPinnedPre25V2Revision() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyPre25V2Fixture(pack.path()));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    QCOMPARE(loaded->manifest_schema_version, std::uint32_t{2});
    QCOMPARE(loaded->revision.digest,
             std::string("bb5e15c14407788a7d9e5370efa610cd12e84a09ca598781bc2f37210f1d4f8d"));

    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    QCOMPARE(runtime->cases.front().argument_configurations.size(), std::size_t{1});
    QVERIFY(
        !runtime->cases.front().argument_configurations.front().grounded_question_bank.has_value());
}

void SchemaDispatchTest::loadsV2AndProjectsRuntime() {
    const auto v1 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    const auto v2 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(v1.has_value(), v1 ? "" : qPrintable(v1.error().message));
    QVERIFY2(v2.has_value(), v2 ? "" : qPrintable(v2.error().message));
    QCOMPARE(v2->manifest_schema_version, std::uint32_t{2});
    QCOMPARE(v2->revision.digest,
             std::string("023008f685d42634a271a626d5df1eb770ee5a6141a1b199eaa6d9945c4f15ce"));
    QVERIFY(v2->revision.digest != v1->revision.digest);
    for (const auto& resource : v2->resources) {
        QCOMPARE(resource.descriptor.schema_version, std::uint32_t{2});
    }
    const auto runtime = appellate::packs::loadRuntimePack(*v2);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    QCOMPARE(runtime->cases.front().definition.id.value, std::string("example.case.fictional"));
    const auto& definition = runtime->cases.front().definition;
    QCOMPARE(definition.disposition_targets.size(), std::size_t{2});
    QCOMPARE(definition.disposition_targets.front().issue_id.value,
             std::string("example.issue.preservation"));
    QCOMPARE(definition.disposition_targets.front().target_id.value,
             std::string("example.target.preservation"));
    QCOMPARE(definition.disposition_plans.size(), std::size_t{1});
    QCOMPARE(definition.disposition_plans.front().canonical_sha256,
             std::string("eb57e60742d575427f440eb816575a2bc2bd214c2068d79e7a9a7beba2d51a66"));
    QCOMPARE(definition.disposition_plans.front().components.size(), std::size_t{2});
    QCOMPARE(definition.disposition_plans.front().components.front().action,
             appellate::model::DispositionAction::Dismiss);
    QVERIFY(definition.disposition_plans.front().components.front().remand);
    QVERIFY(definition.authored_disposition_plan_id.has_value());
    QCOMPARE(definition.authored_disposition_plan_id->value,
             std::string("example.disposition.fictional"));
    QVERIFY(definition.authored_disposition_operation_id.has_value());
    QCOMPARE(definition.authored_disposition_operation_id->value,
             std::string("example.operation.issue-judgment"));
    const auto judgment =
        std::ranges::find(runtime->cases.front().workflow.operations,
                          appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
                          &appellate::model::WorkflowOperation::id);
    QVERIFY(judgment != runtime->cases.front().workflow.operations.end());
    QVERIFY(!judgment->disposition_plan_id.has_value());
    QCOMPARE(judgment->preconditions.size(), std::size_t{1});
    const auto* filing_precondition =
        std::get_if<appellate::model::WorkflowFilingPrecondition>(&judgment->preconditions.front());
    QVERIFY(filing_precondition != nullptr);
    QCOMPARE(filing_precondition->filing_type.value, std::string("example.filing.notice"));
    QVERIFY(filing_precondition->present);
    const auto& authority = runtime->cases.front().workflow.operations.front().authority.primary;
    QCOMPARE(authority.id.value, std::string("example.authority.filing-eligibility"));
    QCOMPARE(authority.citation, std::string("Fictional Rule 2"));
    QCOMPARE(authority.source_version, std::string("2026-01-01"));
    QCOMPARE(authority.proposition,
             std::string("A filing that is not eligible at the current stage is rejected."));
    QVERIFY(authority.provenance.has_value());
    QCOMPARE(authority.provenance->type, appellate::model::AuthorityType::Rule);
    QCOMPARE(authority.provenance->jurisdiction_id, std::string("example.jurisdiction.fictional"));
    QCOMPARE(authority.provenance->issuing_body_id, std::string("example.court.fictional"));
    QCOMPARE(authority.provenance->precedential_status,
             appellate::model::PrecedentialStatus::NotApplicable);
    QCOMPARE(authority.provenance->official_source, true);
    QCOMPARE(authority.provenance->checked_on, std::string("2026-01-01"));
    QCOMPARE(authority.provenance->locator, std::string("Rule 2"));
    QCOMPARE(authority.provenance->source_url, std::string("https://example.invalid/rules/2"));

    QCOMPARE(runtime->cases.front().argument_configurations.size(), std::size_t{2});
    const auto actual =
        std::ranges::find(runtime->cases.front().argument_configurations,
                          appellate::packs::RuntimeArgumentConfigId{"example.argument.fictional"},
                          &appellate::packs::RuntimeArgumentConfiguration::id);
    const auto counterfactual = std::ranges::find(
        runtime->cases.front().argument_configurations,
        appellate::packs::RuntimeArgumentConfigId{"example.argument.counterfactual"},
        &appellate::packs::RuntimeArgumentConfiguration::id);
    QVERIFY(actual != runtime->cases.front().argument_configurations.end());
    QVERIFY(counterfactual != runtime->cases.front().argument_configurations.end());
    QVERIFY(actual->grounded_question_bank.has_value());
    QVERIFY(counterfactual->grounded_question_bank.has_value());

    const auto& actual_bank = *actual->grounded_question_bank;
    QCOMPARE(actual_bank.case_id.value, std::string("example.case.fictional"));
    QCOMPARE(actual_bank.argument_configuration_id, std::string("example.argument.fictional"));
    QCOMPARE(actual_bank.mode, appellate::model::OralArgumentMode::ActualRecord);
    QCOMPARE(actual_bank.grounding_digest,
             std::string("766b0a05b8d4c6ed2b05496f520bc34d11ade1d1d670f7dd6fb036c11a238c55"));
    QCOMPARE(actual_bank.issue_topics.size(), std::size_t{2});
    QCOMPARE(actual_bank.questions.size(), std::size_t{4});
    const auto preservation =
        std::ranges::find(actual_bank.questions, std::string("example.question.preservation"),
                          &appellate::model::AuthoredArgumentQuestion::id);
    QVERIFY(preservation != actual_bank.questions.end());
    QCOMPARE(preservation->topic, appellate::model::ArgumentFocusTopic::Preservation);
    QCOMPARE(preservation->grounding.size(), std::size_t{3});
    const auto authority_grounding =
        std::ranges::find_if(preservation->grounding, [](const auto& grounding) {
            return std::holds_alternative<appellate::model::AuthorityArgumentGrounding>(grounding);
        });
    const auto brief_grounding =
        std::ranges::find_if(preservation->grounding, [](const auto& grounding) {
            return std::holds_alternative<appellate::model::BriefPageArgumentGrounding>(grounding);
        });
    const auto record_grounding =
        std::ranges::find_if(preservation->grounding, [](const auto& grounding) {
            return std::holds_alternative<appellate::model::RecordPageArgumentGrounding>(grounding);
        });
    QVERIFY(authority_grounding != preservation->grounding.end());
    QVERIFY(brief_grounding != preservation->grounding.end());
    QVERIFY(record_grounding != preservation->grounding.end());
    const auto& grounded_authority =
        std::get<appellate::model::AuthorityArgumentGrounding>(*authority_grounding).authority;
    QCOMPARE(grounded_authority.id.value, std::string("example.authority.rule-one"));
    QCOMPARE(grounded_authority.citation, std::string("Fictional Rule 1"));
    QVERIFY(grounded_authority.provenance.has_value());
    QCOMPARE(grounded_authority.provenance->source_url,
             std::string("https://example.invalid/rules/1"));
    const auto& grounded_brief =
        std::get<appellate::model::BriefPageArgumentGrounding>(*brief_grounding);
    QCOMPARE(grounded_brief.record_entry_id, std::string("example.record.brief-opening"));
    QCOMPARE(grounded_brief.page_number, std::uint32_t{2});
    QCOMPARE(grounded_brief.asset_sha256,
             std::string("bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366"));
    const auto& grounded_record =
        std::get<appellate::model::RecordPageArgumentGrounding>(*record_grounding);
    QCOMPARE(grounded_record.record_anchor_id, std::string("example.record.anchor.ja2"));
    QCOMPARE(grounded_record.record_entry_id, std::string("example.record.entry-one"));
    QCOMPARE(grounded_record.page_number, std::uint32_t{2});
    QCOMPARE(grounded_record.asset_sha256,
             std::string("bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366"));
    QCOMPARE(grounded_record.citation_label, std::optional<std::string>{"JA2"});

    const auto& counterfactual_bank = *counterfactual->grounded_question_bank;
    QCOMPARE(counterfactual_bank.mode, appellate::model::OralArgumentMode::CounterfactualTraining);
    QCOMPARE(counterfactual_bank.grounding_digest,
             std::string("398c9797ae359c4a317ababe2db7e27c1dad8b11d4059f36a71d01262edf11d5"));
    QCOMPARE(counterfactual_bank.issue_topics.size(), std::size_t{2});
    QCOMPARE(counterfactual_bank.questions.size(), std::size_t{2});
}

void SchemaDispatchTest::allowsProcedureAuthoritySetsBeyondCourtDefaults() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(addCaseAuthoritySet(pack.path()));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto find_resource = [](auto& candidate, appellate::model::ResourceKind kind) {
        return std::ranges::find_if(candidate.resources, [kind](const auto& resource) {
            return resource.descriptor.kind == kind;
        });
    };
    const auto court = find_resource(*loaded, appellate::model::ResourceKind::Court);
    const auto procedure = find_resource(*loaded, appellate::model::ResourceKind::ProcedureProfile);
    QVERIFY(court != loaded->resources.end());
    QVERIFY(procedure != loaded->resources.end());
    QCOMPARE(court->document.value(QStringLiteral("authority_set_ids")).toArray(),
             QJsonArray{QStringLiteral("example.authorities.fictional")});
    QCOMPARE(procedure->document.value(QStringLiteral("authority_set_ids")).toArray(),
             (QJsonArray{QStringLiteral("example.authorities.fictional"),
                         QStringLiteral("example.authorities.case-specific")}));

    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& issue_authorities = runtime->cases.front().issues.front().authorities;
    const auto case_authority = std::ranges::find(
        issue_authorities, appellate::model::AuthorityId{"example.authority.case-specific"},
        &appellate::model::AuthorityRef::id);
    QVERIFY(case_authority != issue_authorities.end());
    QVERIFY(case_authority->provenance.has_value());
    QCOMPARE(case_authority->provenance->source_url,
             std::string("https://example.invalid/rules/7"));

    const auto rejects_graph_and_runtime = [](const appellate::packs::LoadedPack& candidate,
                                              appellate::packs::RuntimePackErrorCode runtime_code) {
        const auto graph = PackReader::validateResolvedGraph(candidate, {});
        const auto projected = appellate::packs::loadRuntimePack(candidate);
        return !graph.has_value() && graph.error().code == ErrorCode::CrossReferenceFailure &&
               !projected.has_value() && projected.error().code == runtime_code;
    };

    auto outside_procedure = *loaded;
    auto outside_procedure_profile =
        find_resource(outside_procedure, appellate::model::ResourceKind::ProcedureProfile);
    QVERIFY(outside_procedure_profile != outside_procedure.resources.end());
    outside_procedure_profile->document.insert(
        QStringLiteral("authority_set_ids"),
        QJsonArray{QStringLiteral("example.authorities.fictional")});
    QVERIFY(rejects_graph_and_runtime(
        outside_procedure, appellate::packs::RuntimePackErrorCode::CrossReferenceFailure));

    auto unknown_set = *loaded;
    auto unknown_set_procedure =
        find_resource(unknown_set, appellate::model::ResourceKind::ProcedureProfile);
    QVERIFY(unknown_set_procedure != unknown_set.resources.end());
    auto set_ids =
        unknown_set_procedure->document.value(QStringLiteral("authority_set_ids")).toArray();
    set_ids.push_back(QStringLiteral("example.authorities.unknown"));
    unknown_set_procedure->document.insert(QStringLiteral("authority_set_ids"), set_ids);
    QVERIFY(rejects_graph_and_runtime(unknown_set,
                                      appellate::packs::RuntimePackErrorCode::MissingResource));

    auto unknown_authority = *loaded;
    auto unknown_authority_case =
        find_resource(unknown_authority, appellate::model::ResourceKind::Case);
    QVERIFY(unknown_authority_case != unknown_authority.resources.end());
    auto issues = unknown_authority_case->document.value(QStringLiteral("issues")).toArray();
    auto issue = issues.first().toObject();
    auto authority_ids = issue.value(QStringLiteral("authority_ids")).toArray();
    authority_ids.push_back(QStringLiteral("example.authority.unknown"));
    issue.insert(QStringLiteral("authority_ids"), authority_ids);
    issues.replace(0, issue);
    unknown_authority_case->document.insert(QStringLiteral("issues"), issues);
    QVERIFY(rejects_graph_and_runtime(unknown_authority,
                                      appellate::packs::RuntimePackErrorCode::MissingResource));
}

void SchemaDispatchTest::validatesSealedRecordTwins() {
    QTemporaryDir valid_pack;
    QVERIFY(valid_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), valid_pack.path()));
    QVERIFY(addSealedRecordTwins(valid_pack.path()));
    const auto loaded = PackReader::readDirectory(valid_pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& record = runtime->cases.front().record;
    QVERIFY(record.disclosure_policy.has_value());
    QCOMPARE(record.disclosure_policy->policy_id, std::string("example.record.policy.psr"));
    QCOMPARE(record.sealed_disclosures.size(), std::size_t{1});
    const auto& disclosure = record.sealed_disclosures.front();
    QCOMPARE(disclosure.disclosure_id.value, std::string("example.disclosure.psr"));
    QCOMPARE(disclosure.sealed_entry_id.value, std::string("example.record.psr-sealed"));
    QCOMPARE(disclosure.authorization_authority_id.value,
             std::string("example.authority.rule-one"));
    QCOMPARE(disclosure.anchor_mappings.front().stable_anchor_id.value,
             std::string("example.record.anchor.psr-stable"));

    QTemporaryDir stable_issue_pack;
    QVERIFY(stable_issue_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), stable_issue_pack.path()));
    QVERIFY(addSealedRecordTwins(stable_issue_pack.path()));
    QVERIFY(mutateResource(
        stable_issue_pack.path(), QStringLiteral("resources/case.json"), [](QJsonObject& document) {
            auto issues = document.value(QStringLiteral("issues")).toArray();
            auto issue = issues.at(0).toObject();
            auto anchors = issue.value(QStringLiteral("record_anchor_ids")).toArray();
            anchors.push_back(QStringLiteral("example.record.anchor.psr-stable"));
            issue.insert(QStringLiteral("record_anchor_ids"), anchors);
            issues.replace(0, issue);
            document.insert(QStringLiteral("issues"), issues);
        }));
    const auto stable_issue = PackReader::readDirectory(stable_issue_pack.path());
    QVERIFY2(stable_issue.has_value(),
             stable_issue ? "" : qPrintable(stable_issue.error().message));
    QVERIFY(appellate::packs::loadRuntimePack(*stable_issue).has_value());

    const auto introduce_ordered_identity_collision = [](QJsonObject& document) {
        auto entries = document.value(QStringLiteral("docket_entries")).toArray();
        auto second_sealed = entries.at(0).toObject();
        second_sealed.insert(QStringLiteral("entry_id"),
                             QStringLiteral("example.record.second-sealed"));
        second_sealed.insert(QStringLiteral("entry_number"), 4);
        second_sealed.insert(QStringLiteral("entry_label"), QStringLiteral("ECF No. 43-S"));
        second_sealed.insert(QStringLiteral("title"), QStringLiteral("Second sealed filing"));
        second_sealed.insert(QStringLiteral("sealed"), true);
        entries.push_back(second_sealed);
        document.insert(QStringLiteral("docket_entries"), entries);

        auto disclosures = document.value(QStringLiteral("sealed_disclosures")).toArray();
        disclosures.push_back(QJsonObject{
            {QStringLiteral("disclosure_id"), QStringLiteral("example.record.anchor.psr-stable")},
            {QStringLiteral("sealed_entry_id"), QStringLiteral("example.record.second-sealed")},
            {QStringLiteral("authorization_authority_id"),
             QStringLiteral("example.authority.rule-one")},
            {QStringLiteral("required_items"), QJsonArray{}},
            {QStringLiteral("anchor_mappings"), QJsonArray{}},
        });
        document.insert(QStringLiteral("sealed_disclosures"), disclosures);
    };

    for (int variant = 0; variant < 7; ++variant) {
        QTemporaryDir invalid_pack;
        QVERIFY(invalid_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), invalid_pack.path()));
        QVERIFY(addSealedRecordTwins(invalid_pack.path()));
        if (variant == 0) {
            QVERIFY(mutateManifest(invalid_pack.path(), [](QJsonObject& manifest) {
                QJsonArray retained;
                for (const auto& value :
                     manifest.value(QStringLiteral("required_capabilities")).toArray()) {
                    if (value.toObject().value(QStringLiteral("id")).toString() !=
                        QStringLiteral("workbench.pack.sealed-record-twins")) {
                        retained.push_back(value);
                    }
                }
                manifest.insert(QStringLiteral("required_capabilities"), retained);
            }));
        } else if (variant == 1) {
            QVERIFY(mutateResource(invalid_pack.path(), QStringLiteral("resources/record.json"),
                                   [](QJsonObject& document) {
                                       document.remove(QStringLiteral("disclosure_policy"));
                                   }));
        } else if (variant == 2) {
            QVERIFY(mutateResource(
                invalid_pack.path(), QStringLiteral("resources/record.json"),
                [](QJsonObject& document) {
                    auto disclosures =
                        document.value(QStringLiteral("sealed_disclosures")).toArray();
                    auto invalid_disclosure = disclosures.at(0).toObject();
                    invalid_disclosure.insert(QStringLiteral("authorization_authority_id"),
                                              QStringLiteral("example.authority.missing"));
                    disclosures.replace(0, invalid_disclosure);
                    document.insert(QStringLiteral("sealed_disclosures"), disclosures);
                }));
        } else if (variant == 3 || variant == 4) {
            QVERIFY(mutateResource(
                invalid_pack.path(), QStringLiteral("resources/case.json"),
                [variant](QJsonObject& document) {
                    auto issues = document.value(QStringLiteral("issues")).toArray();
                    auto issue = issues.at(0).toObject();
                    auto anchors = issue.value(QStringLiteral("record_anchor_ids")).toArray();
                    anchors.push_back(variant == 3
                                          ? QStringLiteral("example.record.anchor.psr-sealed")
                                          : QStringLiteral("example.record.anchor.psr-unmapped"));
                    issue.insert(QStringLiteral("record_anchor_ids"), anchors);
                    issues.replace(0, issue);
                    document.insert(QStringLiteral("issues"), issues);
                }));
        } else if (variant == 5) {
            QVERIFY(mutateResource(
                invalid_pack.path(), QStringLiteral("resources/record.json"),
                [](QJsonObject& document) {
                    auto entries = document.value(QStringLiteral("docket_entries")).toArray();
                    auto public_entry = entries.at(0).toObject();
                    public_entry.insert(QStringLiteral("parent_entry_id"),
                                        QStringLiteral("example.record.psr-sealed"));
                    public_entry.insert(QStringLiteral("relationship"),
                                        QStringLiteral("supplement"));
                    entries.replace(0, public_entry);
                    document.insert(QStringLiteral("docket_entries"), entries);
                }));
        } else {
            QVERIFY(mutateResource(invalid_pack.path(), QStringLiteral("resources/record.json"),
                                   introduce_ordered_identity_collision));
        }
        const auto rejected = PackReader::readDirectory(invalid_pack.path());
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, variant == 0 ? ErrorCode::UnsupportedCapability
                                                     : ErrorCode::CrossReferenceFailure);
    }

    auto forged = *loaded;
    const auto record_resource = std::ranges::find_if(
        forged.resources, [](const appellate::packs::ValidatedResource& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
    QVERIFY(record_resource != forged.resources.end());
    auto disclosures =
        record_resource->document.value(QStringLiteral("sealed_disclosures")).toArray();
    auto forged_disclosure = disclosures.at(0).toObject();
    forged_disclosure.insert(QStringLiteral("authorization_authority_id"),
                             QStringLiteral("example.authority.forged"));
    disclosures.replace(0, forged_disclosure);
    record_resource->document.insert(QStringLiteral("sealed_disclosures"), disclosures);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto forged_unmapped_anchor = *loaded;
    const auto case_resource = std::ranges::find_if(
        forged_unmapped_anchor.resources, [](const appellate::packs::ValidatedResource& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Case;
        });
    QVERIFY(case_resource != forged_unmapped_anchor.resources.end());
    auto issues = case_resource->document.value(QStringLiteral("issues")).toArray();
    auto issue = issues.at(0).toObject();
    auto issue_anchors = issue.value(QStringLiteral("record_anchor_ids")).toArray();
    issue_anchors.push_back(QStringLiteral("example.record.anchor.psr-unmapped"));
    issue.insert(QStringLiteral("record_anchor_ids"), issue_anchors);
    issues.replace(0, issue);
    case_resource->document.insert(QStringLiteral("issues"), issues);
    const auto forged_unmapped_runtime = appellate::packs::loadRuntimePack(forged_unmapped_anchor);
    QVERIFY(!forged_unmapped_runtime.has_value());
    QCOMPARE(forged_unmapped_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto forged_parent = *loaded;
    const auto forged_parent_record = std::ranges::find_if(
        forged_parent.resources, [](const appellate::packs::ValidatedResource& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
    QVERIFY(forged_parent_record != forged_parent.resources.end());
    auto entries = forged_parent_record->document.value(QStringLiteral("docket_entries")).toArray();
    auto public_entry = entries.at(0).toObject();
    public_entry.insert(QStringLiteral("parent_entry_id"),
                        QStringLiteral("example.record.psr-sealed"));
    public_entry.insert(QStringLiteral("relationship"), QStringLiteral("supplement"));
    entries.replace(0, public_entry);
    forged_parent_record->document.insert(QStringLiteral("docket_entries"), entries);
    const auto forged_parent_runtime = appellate::packs::loadRuntimePack(forged_parent);
    QVERIFY(!forged_parent_runtime.has_value());
    QCOMPARE(forged_parent_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto forged_identity_collision = *loaded;
    const auto collision_record = std::ranges::find_if(
        forged_identity_collision.resources,
        [](const appellate::packs::ValidatedResource& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
    QVERIFY(collision_record != forged_identity_collision.resources.end());
    introduce_ordered_identity_collision(collision_record->document);
    const auto collision_runtime = appellate::packs::loadRuntimePack(forged_identity_collision);
    QVERIFY(!collision_runtime.has_value());
    QCOMPARE(collision_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::validatesGroundedQuestionBanks() {
    const auto argument_path = QStringLiteral("resources/argument-config.json");
    for (int variant = 0; variant < 6; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), argument_path, [variant](QJsonObject& document) {
            auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
            auto bindings = bank.value(QStringLiteral("issue_topic_bindings")).toArray();
            auto questions = bank.value(QStringLiteral("questions")).toArray();
            if (variant == 0) {
                auto question = questions.at(0).toObject();
                question.insert(QStringLiteral("prompt"),
                                QStringLiteral("Where, exactly, was this issue preserved?"));
                questions.replace(0, question);
            } else if (variant == 1) {
                bindings.removeLast();
            } else if (variant == 2) {
                auto question = questions.at(2).toObject();
                auto grounding = question.value(QStringLiteral("grounding")).toArray();
                auto authority = grounding.at(0).toObject();
                authority.insert(QStringLiteral("authority_id"),
                                 QStringLiteral("example.authority.judgment"));
                grounding.replace(0, authority);
                question.insert(QStringLiteral("grounding"), grounding);
                questions.replace(2, question);
            } else if (variant == 3) {
                auto question = questions.at(0).toObject();
                auto grounding = question.value(QStringLiteral("grounding")).toArray();
                auto brief = grounding.at(1).toObject();
                brief.insert(QStringLiteral("page_number"), 4);
                grounding.replace(1, brief);
                question.insert(QStringLiteral("grounding"), grounding);
                questions.replace(0, question);
            } else if (variant == 4) {
                auto first = questions.at(0).toObject();
                const auto first_id = first.value(QStringLiteral("grounding"))
                                          .toArray()
                                          .at(0)
                                          .toObject()
                                          .value(QStringLiteral("grounding_id"));
                auto second = questions.at(1).toObject();
                auto grounding = second.value(QStringLiteral("grounding")).toArray();
                auto reference = grounding.at(0).toObject();
                reference.insert(QStringLiteral("grounding_id"), first_id);
                grounding.replace(0, reference);
                second.insert(QStringLiteral("grounding"), grounding);
                questions.replace(1, second);
            } else {
                questions.removeAt(1);
            }
            bank.insert(QStringLiteral("issue_topic_bindings"), bindings);
            bank.insert(QStringLiteral("questions"), questions);
            document.insert(QStringLiteral("grounded_question_bank"), bank);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::CrossReferenceFailure);
    }

    for (int variant = 0; variant < 2; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), argument_path, [variant](QJsonObject& document) {
            auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
            auto questions = bank.value(QStringLiteral("questions")).toArray();
            auto question = questions.at(0).toObject();
            auto grounding = question.value(QStringLiteral("grounding")).toArray();
            auto reference = grounding.at(1).toObject();
            if (variant == 0) {
                reference.remove(QStringLiteral("page_number"));
            } else {
                reference.insert(QStringLiteral("anchor_id"),
                                 QStringLiteral("example.record.anchor.ja2"));
            }
            grounding.replace(1, reference);
            question.insert(QStringLiteral("grounding"), grounding);
            questions.replace(0, question);
            bank.insert(QStringLiteral("questions"), questions);
            document.insert(QStringLiteral("grounded_question_bank"), bank);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
    }

    QTemporaryDir noncanonical_focus;
    QVERIFY(noncanonical_focus.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), noncanonical_focus.path()));
    QVERIFY(mutateResource(
        noncanonical_focus.path(), QStringLiteral("resources/judge-profile.json"),
        [](QJsonObject& document) {
            auto interaction = document.value(QStringLiteral("interaction")).toObject();
            auto focus = interaction.value(QStringLiteral("issue_focus")).toArray();
            auto item = focus.at(0).toObject();
            item.insert(QStringLiteral("topic_id"), QStringLiteral("example.topic.pack-authored"));
            focus.replace(0, item);
            interaction.insert(QStringLiteral("issue_focus"), focus);
            document.insert(QStringLiteral("interaction"), interaction);
        }));
    const auto rejected_focus = PackReader::readDirectory(noncanonical_focus.path());
    QVERIFY(!rejected_focus.has_value());
    QCOMPARE(rejected_focus.error().code, ErrorCode::CrossReferenceFailure);

    for (int variant = 0; variant < 2; ++variant) {
        QTemporaryDir unlaunchable_focus;
        QVERIFY(unlaunchable_focus.isValid());
        QVERIFY(
            copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unlaunchable_focus.path()));
        QVERIFY(mutateResource(
            unlaunchable_focus.path(), QStringLiteral("resources/judge-profile.json"),
            [variant](QJsonObject& document) {
                auto interaction = document.value(QStringLiteral("interaction")).toObject();
                auto focus = interaction.value(QStringLiteral("issue_focus")).toArray();
                for (qsizetype index = 0; index < focus.size(); ++index) {
                    auto item = focus.at(index).toObject();
                    if (variant == 0) {
                        static const std::array disjoint_topics{
                            QStringLiteral("workbench.topic.jurisdiction"),
                            QStringLiteral("workbench.topic.standard-of-review"),
                            QStringLiteral("workbench.topic.governing-authority"),
                            QStringLiteral("workbench.topic.practical-consequences"),
                        };
                        item.insert(QStringLiteral("topic_id"),
                                    disjoint_topics.at(static_cast<std::size_t>(index)));
                    } else {
                        item.insert(QStringLiteral("weight"), 0.0);
                    }
                    focus.replace(index, item);
                }
                interaction.insert(QStringLiteral("issue_focus"), focus);
                document.insert(QStringLiteral("interaction"), interaction);
            }));
        const auto rejected = PackReader::readDirectory(unlaunchable_focus.path());
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, ErrorCode::CrossReferenceFailure);
    }

    for (int variant = 0; variant < 2; ++variant) {
        QTemporaryDir record_pack;
        QVERIFY(record_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), record_pack.path()));
        QVERIFY(mutateResource(record_pack.path(), QStringLiteral("resources/record.json"),
                               [variant](QJsonObject& document) {
                                   auto entries =
                                       document.value(QStringLiteral("docket_entries")).toArray();
                                   auto entry = entries.at(variant == 0 ? 1 : 0).toObject();
                                   if (variant == 0) {
                                       entry.insert(QStringLiteral("tags"),
                                                    QJsonArray{QStringLiteral("opening-brief")});
                                   } else {
                                       entry.insert(QStringLiteral("sealed"), true);
                                   }
                                   entries.replace(variant == 0 ? 1 : 0, entry);
                                   document.insert(QStringLiteral("docket_entries"), entries);
                               }));
        const auto rejected_record = PackReader::readDirectory(record_pack.path());
        QVERIFY(!rejected_record.has_value());
        QCOMPARE(rejected_record.error().code, ErrorCode::CrossReferenceFailure);
    }

    for (int scalar_count : {120, 121}) {
        QTemporaryDir label_pack;
        QVERIFY(label_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), label_pack.path()));
        QVERIFY(mutateResource(label_pack.path(), QStringLiteral("resources/record.json"),
                               [scalar_count](QJsonObject& document) {
                                   auto anchors =
                                       document.value(QStringLiteral("page_anchors")).toArray();
                                   auto anchor = anchors.at(0).toObject();
                                   anchor.insert(QStringLiteral("citation_label"),
                                                 QString(scalar_count, QChar{0xD55C}));
                                   anchors.replace(0, anchor);
                                   document.insert(QStringLiteral("page_anchors"), anchors);
                               }));
        QVERIFY(refreshQuestionBankDigest(label_pack.path(), argument_path));
        QVERIFY(refreshQuestionBankDigest(
            label_pack.path(), QStringLiteral("resources/argument-config-counterfactual.json")));
        const auto result = PackReader::readDirectory(label_pack.path());
        if (scalar_count == 120) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
            const auto runtime = appellate::packs::loadRuntimePack(*result);
            QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
        } else {
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
        }
    }

    QTemporaryDir per_issue_overflow;
    QVERIFY(per_issue_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), per_issue_overflow.path()));
    QVERIFY(mutateResource(per_issue_overflow.path(), argument_path, [](QJsonObject& document) {
        auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
        auto questions = bank.value(QStringLiteral("questions")).toArray();
        const auto template_question = questions.at(0).toObject();
        for (int index = 1; index < 32; ++index) {
            auto question = template_question;
            question.insert(QStringLiteral("question_id"),
                            QStringLiteral("example.question.boundary-%1").arg(index));
            auto grounding = question.value(QStringLiteral("grounding")).toArray();
            for (qsizetype grounding_index = 0; grounding_index < grounding.size();
                 ++grounding_index) {
                auto reference = grounding.at(grounding_index).toObject();
                reference.insert(QStringLiteral("grounding_id"),
                                 QStringLiteral("example.grounding.boundary-%1-%2")
                                     .arg(index)
                                     .arg(grounding_index));
                grounding.replace(grounding_index, reference);
            }
            question.insert(QStringLiteral("grounding"), grounding);
            questions.push_back(question);
        }
        bank.insert(QStringLiteral("questions"), questions);
        document.insert(QStringLiteral("grounded_question_bank"), bank);
    }));
    const auto overflow = PackReader::readDirectory(per_issue_overflow.path());
    QVERIFY(!overflow.has_value());
    QCOMPARE(overflow.error().code, ErrorCode::CrossReferenceFailure);

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto argument = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.id == "example.argument.fictional";
    });
    QVERIFY(argument != forged.resources.end());
    auto forged_bank =
        argument->document.value(QStringLiteral("grounded_question_bank")).toObject();
    forged_bank.insert(QStringLiteral("grounding_digest"), QString(64, QLatin1Char('0')));
    argument->document.insert(QStringLiteral("grounded_question_bank"), forged_bank);
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto forged_focus = *loaded;
    const auto forged_profile =
        std::ranges::find_if(forged_focus.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::JudgeProfile;
        });
    QVERIFY(forged_profile != forged_focus.resources.end());
    auto interaction = forged_profile->document.value(QStringLiteral("interaction")).toObject();
    auto focus = interaction.value(QStringLiteral("issue_focus")).toArray();
    for (qsizetype index = 0; index < focus.size(); ++index) {
        auto item = focus.at(index).toObject();
        item.insert(QStringLiteral("topic_id"), QStringLiteral("workbench.topic.jurisdiction"));
        focus.replace(index, item);
    }
    interaction.insert(QStringLiteral("issue_focus"), focus);
    forged_profile->document.insert(QStringLiteral("interaction"), interaction);
    for (auto& profile : forged_focus.judge_profiles) {
        for (auto& item : profile.interaction.issue_focus) {
            item.topic_id = "workbench.topic.jurisdiction";
        }
    }
    const auto forged_focus_runtime = appellate::packs::loadRuntimePack(forged_focus);
    QVERIFY(!forged_focus_runtime.has_value());
    QCOMPARE(forged_focus_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    auto forged_label = *loaded;
    const auto forged_record =
        std::ranges::find_if(forged_label.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Record;
        });
    QVERIFY(forged_record != forged_label.resources.end());
    auto anchors = forged_record->document.value(QStringLiteral("page_anchors")).toArray();
    auto first_anchor = anchors.at(0).toObject();
    first_anchor.insert(QStringLiteral("citation_label"), QStringLiteral("JA\n2"));
    anchors.replace(0, first_anchor);
    forged_record->document.insert(QStringLiteral("page_anchors"), anchors);
    const auto forged_label_runtime = appellate::packs::loadRuntimePack(forged_label);
    QVERIFY(!forged_label_runtime.has_value());
    QCOMPARE(forged_label_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::InvalidResource);

    auto root = *loaded;
    auto dependency = *loaded;
    root.revision.id.value = "example.grounded.owner-root";
    root.revision.digest = std::string(64, 'a');
    dependency.revision.id.value = "example.grounded.owner-dependency";
    dependency.revision.digest = std::string(64, 'b');
    std::erase_if(root.resources, [](const auto& resource) {
        return resource.descriptor.kind != appellate::model::ResourceKind::ArgumentConfig;
    });
    std::erase_if(dependency.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::ArgumentConfig;
    });
    root.blobs.clear();
    root.judge_profiles.clear();
    root.graph_state = appellate::packs::PackGraphState::DeferredReferences;
    dependency.graph_state = appellate::packs::PackGraphState::StandaloneValidated;
    const std::array<const appellate::packs::LoadedPack*, 1> dependency_closure{&dependency};
    const auto wrong_owner = PackReader::validateResolvedGraph(root, dependency_closure);
    QVERIFY(!wrong_owner.has_value());
    QCOMPARE(wrong_owner.error().code, ErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::normalizesGroundedQuestionBankOrdering() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(mutateResource(
        pack.path(), QStringLiteral("resources/argument-config.json"), [](QJsonObject& document) {
            const auto reversed = [](const QJsonArray& values) {
                QJsonArray result;
                for (qsizetype index = values.size(); index > 0; --index) {
                    result.push_back(values.at(index - 1));
                }
                return result;
            };
            auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
            auto bindings = reversed(bank.value(QStringLiteral("issue_topic_bindings")).toArray());
            for (qsizetype index = 0; index < bindings.size(); ++index) {
                auto binding = bindings.at(index).toObject();
                auto topics = reversed(binding.value(QStringLiteral("topic_ids")).toArray());
                binding.insert(QStringLiteral("topic_ids"), topics);
                bindings.replace(index, binding);
            }
            auto questions = reversed(bank.value(QStringLiteral("questions")).toArray());
            for (qsizetype index = 0; index < questions.size(); ++index) {
                auto question = questions.at(index).toObject();
                auto grounding = reversed(question.value(QStringLiteral("grounding")).toArray());
                question.insert(QStringLiteral("grounding"), grounding);
                questions.replace(index, question);
            }
            bank.insert(QStringLiteral("issue_topic_bindings"), bindings);
            bank.insert(QStringLiteral("questions"), questions);
            document.insert(QStringLiteral("grounded_question_bank"), bank);
        }));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto configuration =
        std::ranges::find(runtime->cases.front().argument_configurations,
                          appellate::packs::RuntimeArgumentConfigId{"example.argument.fictional"},
                          &appellate::packs::RuntimeArgumentConfiguration::id);
    QVERIFY(configuration != runtime->cases.front().argument_configurations.end());
    QVERIFY(configuration->grounded_question_bank.has_value());
    const auto& bank = *configuration->grounded_question_bank;
    QCOMPARE(bank.grounding_digest,
             std::string("766b0a05b8d4c6ed2b05496f520bc34d11ade1d1d670f7dd6fb036c11a238c55"));
    QCOMPARE(bank.issue_topics.front().issue_id, std::string("example.issue.prejudice"));
    QCOMPARE(bank.questions.front().id, std::string("example.question.prejudice"));
    const auto& preservation = bank.questions.back();
    QCOMPARE(preservation.id, std::string("example.question.remedy"));
}

void SchemaDispatchTest::enforcesGroundedQuestionBoundsAndPrompts() {
    const auto argument_path = QStringLiteral("resources/argument-config.json");
    const auto issue_id = [](int index) {
        if (index == 0) {
            return QStringLiteral("example.issue.preservation");
        }
        if (index == 1) {
            return QStringLiteral("example.issue.prejudice");
        }
        return QStringLiteral("example.issue.grounded-boundary-%1").arg(index);
    };
    const auto configure_questions = [&](const QString& root, int full_issue_count,
                                         int questions_per_issue, bool add_extra_issue) {
        const auto total_issue_count = full_issue_count + (add_extra_issue ? 1 : 0);
        if (!mutateResource(
                root, QStringLiteral("resources/case.json"), [&](QJsonObject& document) {
                    auto issues = document.value(QStringLiteral("issues")).toArray();
                    const auto template_issue = issues.at(1).toObject();
                    for (int index = 2; index < total_issue_count; ++index) {
                        auto issue = template_issue;
                        issue.insert(QStringLiteral("issue_id"), issue_id(index));
                        issue.insert(QStringLiteral("title"),
                                     QStringLiteral("Grounded boundary issue %1").arg(index));
                        issue.insert(
                            QStringLiteral("target_ids"),
                            QJsonArray{QStringLiteral("example.target.grounded-%1").arg(index)});
                        issues.push_back(issue);
                    }
                    document.insert(QStringLiteral("issues"), issues);
                })) {
            return false;
        }
        if (!mutateResource(root, argument_path, [&](QJsonObject& document) {
                QJsonArray permitted;
                QJsonArray bindings;
                QJsonArray questions;
                for (int issue_index = 0; issue_index < total_issue_count; ++issue_index) {
                    const auto current_issue = issue_id(issue_index);
                    permitted.push_back(current_issue);
                    bindings.push_back(QJsonObject{
                        {QStringLiteral("issue_id"), current_issue},
                        {QStringLiteral("topic_ids"),
                         QJsonArray{QStringLiteral("workbench.topic.preservation")}},
                    });
                    const auto question_count =
                        add_extra_issue && issue_index == total_issue_count - 1
                            ? 1
                            : questions_per_issue;
                    for (int question_index = 0; question_index < question_count;
                         ++question_index) {
                        questions.push_back(QJsonObject{
                            {QStringLiteral("question_id"),
                             QStringLiteral("example.question.boundary-%1-%2")
                                 .arg(issue_index)
                                 .arg(question_index)},
                            {QStringLiteral("issue_id"), current_issue},
                            {QStringLiteral("topic_id"),
                             QStringLiteral("workbench.topic.preservation")},
                            {QStringLiteral("prompt"),
                             QStringLiteral("Grounded boundary question %1, item %2?")
                                 .arg(issue_index)
                                 .arg(question_index)},
                            {QStringLiteral("grounding"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("grounding_id"),
                                  QStringLiteral("example.grounding.boundary-%1-%2")
                                      .arg(issue_index)
                                      .arg(question_index)},
                                 {QStringLiteral("kind"), QStringLiteral("authority")},
                                 {QStringLiteral("authority_id"),
                                  QStringLiteral("example.authority.rule-one")},
                             }}},
                        });
                    }
                }
                document.insert(QStringLiteral("permitted_issue_ids"), permitted);
                auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
                bank.insert(QStringLiteral("issue_topic_bindings"), bindings);
                bank.insert(QStringLiteral("questions"), questions);
                document.insert(QStringLiteral("grounded_question_bank"), bank);
            })) {
            return false;
        }
        return refreshQuestionBankDigest(root, argument_path);
    };

    QTemporaryDir maximum;
    QVERIFY(maximum.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), maximum.path()));
    QVERIFY(configure_questions(maximum.path(), 8, 16, false));
    const auto maximum_loaded = PackReader::readDirectory(maximum.path());
    QVERIFY2(maximum_loaded.has_value(),
             maximum_loaded ? "" : qPrintable(maximum_loaded.error().message));
    const auto maximum_runtime = appellate::packs::loadRuntimePack(*maximum_loaded);
    QVERIFY2(maximum_runtime.has_value(),
             maximum_runtime ? "" : maximum_runtime.error().message.c_str());
    const auto maximum_configuration =
        std::ranges::find(maximum_runtime->cases.front().argument_configurations,
                          appellate::packs::RuntimeArgumentConfigId{"example.argument.fictional"},
                          &appellate::packs::RuntimeArgumentConfiguration::id);
    QVERIFY(maximum_configuration != maximum_runtime->cases.front().argument_configurations.end());
    QVERIFY(maximum_configuration->grounded_question_bank.has_value());
    QCOMPARE(maximum_configuration->grounded_question_bank->questions.size(), std::size_t{128});

    auto forged_overflow = *maximum_loaded;
    const auto forged_argument =
        std::ranges::find_if(forged_overflow.resources, [](const auto& resource) {
            return resource.descriptor.id == "example.argument.fictional";
        });
    QVERIFY(forged_argument != forged_overflow.resources.end());
    auto forged_bank =
        forged_argument->document.value(QStringLiteral("grounded_question_bank")).toObject();
    auto forged_questions = forged_bank.value(QStringLiteral("questions")).toArray();
    auto forged_question = forged_questions.first().toObject();
    forged_question.insert(QStringLiteral("question_id"),
                           QStringLiteral("example.question.forged-overflow"));
    auto forged_grounding = forged_question.value(QStringLiteral("grounding")).toArray();
    auto forged_reference = forged_grounding.first().toObject();
    forged_reference.insert(QStringLiteral("grounding_id"),
                            QStringLiteral("example.grounding.forged-overflow"));
    forged_grounding.replace(0, forged_reference);
    forged_question.insert(QStringLiteral("grounding"), forged_grounding);
    forged_questions.push_back(forged_question);
    forged_bank.insert(QStringLiteral("questions"), forged_questions);
    forged_argument->document.insert(QStringLiteral("grounded_question_bank"), forged_bank);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged_overflow);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);

    QTemporaryDir total_overflow;
    QVERIFY(total_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), total_overflow.path()));
    QVERIFY(configure_questions(total_overflow.path(), 8, 16, true));
    const auto too_many_total = PackReader::readDirectory(total_overflow.path());
    QVERIFY(!too_many_total.has_value());
    QCOMPARE(too_many_total.error().code, ErrorCode::SchemaViolation);

    QTemporaryDir issue_overflow;
    QVERIFY(issue_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), issue_overflow.path()));
    QVERIFY(configure_questions(issue_overflow.path(), 2, 17, false));
    const auto too_many_for_issue = PackReader::readDirectory(issue_overflow.path());
    QVERIFY(!too_many_for_issue.has_value());
    QCOMPARE(too_many_for_issue.error().code, ErrorCode::CrossReferenceFailure);

    for (int grounding_count : {16, 17}) {
        QTemporaryDir grounding_pack;
        QVERIFY(grounding_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), grounding_pack.path()));
        QVERIFY(mutateResource(
            grounding_pack.path(), argument_path, [grounding_count](QJsonObject& document) {
                auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
                auto questions = bank.value(QStringLiteral("questions")).toArray();
                auto question = questions.at(0).toObject();
                QJsonArray grounding;
                for (int index = 0; index < grounding_count; ++index) {
                    grounding.push_back(QJsonObject{
                        {QStringLiteral("grounding_id"),
                         QStringLiteral("example.grounding.reference-boundary-%1").arg(index)},
                        {QStringLiteral("kind"), QStringLiteral("authority")},
                        {QStringLiteral("authority_id"),
                         QStringLiteral("example.authority.rule-one")},
                    });
                }
                question.insert(QStringLiteral("grounding"), grounding);
                questions.replace(0, question);
                bank.insert(QStringLiteral("questions"), questions);
                document.insert(QStringLiteral("grounded_question_bank"), bank);
            }));
        QVERIFY(refreshQuestionBankDigest(grounding_pack.path(), argument_path));
        const auto result = PackReader::readDirectory(grounding_pack.path());
        if (grounding_count == 16) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
            const auto runtime = appellate::packs::loadRuntimePack(*result);
            QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
        } else {
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
        }
    }

    const std::array invalid_prompts{
        QStringLiteral(" leading whitespace"),
        QStringLiteral("trailing whitespace "),
        QStringLiteral("   "),
        QString(3, QChar{0x00A0}),
    };
    for (const auto& prompt : invalid_prompts) {
        QTemporaryDir prompt_pack;
        QVERIFY(prompt_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), prompt_pack.path()));
        QVERIFY(mutateResource(prompt_pack.path(), argument_path, [&prompt](QJsonObject& document) {
            auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
            auto questions = bank.value(QStringLiteral("questions")).toArray();
            auto question = questions.at(0).toObject();
            question.insert(QStringLiteral("prompt"), prompt);
            questions.replace(0, question);
            bank.insert(QStringLiteral("questions"), questions);
            document.insert(QStringLiteral("grounded_question_bank"), bank);
        }));
        QVERIFY(refreshQuestionBankDigest(prompt_pack.path(), argument_path));
        const auto result = PackReader::readDirectory(prompt_pack.path());
        QVERIFY(!result.has_value());
        QVERIFY(result.error().code == ErrorCode::SchemaViolation ||
                result.error().code == ErrorCode::CrossReferenceFailure);
    }

    for (int scalar_count : {512, 513}) {
        QTemporaryDir prompt_pack;
        QVERIFY(prompt_pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), prompt_pack.path()));
        const QString prompt(scalar_count, QChar{0xD55C});
        QVERIFY(mutateResource(prompt_pack.path(), argument_path, [&prompt](QJsonObject& document) {
            auto bank = document.value(QStringLiteral("grounded_question_bank")).toObject();
            auto questions = bank.value(QStringLiteral("questions")).toArray();
            auto question = questions.at(0).toObject();
            question.insert(QStringLiteral("prompt"), prompt);
            questions.replace(0, question);
            bank.insert(QStringLiteral("questions"), questions);
            document.insert(QStringLiteral("grounded_question_bank"), bank);
        }));
        QVERIFY(refreshQuestionBankDigest(prompt_pack.path(), argument_path));
        const auto result = PackReader::readDirectory(prompt_pack.path());
        if (scalar_count == 512) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
            const auto runtime = appellate::packs::loadRuntimePack(*result);
            QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
        } else {
            QVERIFY(!result.has_value());
            QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
        }
    }
}

void SchemaDispatchTest::rejectsUnknownAndMismatchedCapabilities() {
    const std::array capabilities{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.unknown")},
                    {QStringLiteral("version"), 2}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                    {QStringLiteral("version"), 3}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                    {QStringLiteral("version"), 1}},
    };
    for (const auto& capability : capabilities) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateManifest(pack.path(), [&](QJsonObject& manifest) {
            manifest.insert(QStringLiteral("required_capabilities"), QJsonArray{capability});
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::UnsupportedCapability);
    }
}

void SchemaDispatchTest::rejectsUnderdeclaredCapabilities() {
    const auto declarative = QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
        {QStringLiteral("version"), 2},
    };
    const auto judge = QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.judge-profile")},
        {QStringLiteral("version"), 2},
    };
    const auto voice = QJsonObject{
        {QStringLiteral("id"), QStringLiteral("workbench.pack.voice-style")},
        {QStringLiteral("version"), 2},
    };
    const std::array declarations{
        QJsonArray{},
        QJsonArray{voice},
        QJsonArray{declarative},
        QJsonArray{declarative, judge},
        QJsonArray{declarative, voice},
    };

    for (const auto& required_capabilities : declarations) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateManifest(pack.path(), [&](QJsonObject& manifest) {
            manifest.insert(QStringLiteral("required_capabilities"), required_capabilities);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::UnsupportedCapability);
    }
}

void SchemaDispatchTest::preservesLegacyV2WithoutOptionalFeatures() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(mutateResource(pack.path(), QStringLiteral("resources/case.json"),
                           [](QJsonObject& document) {
                               auto issues = document.value(QStringLiteral("issues")).toArray();
                               for (qsizetype index = 0; index < issues.size(); ++index) {
                                   auto issue = issues.at(index).toObject();
                                   issue.remove(QStringLiteral("target_ids"));
                                   issues.replace(index, issue);
                               }
                               document.insert(QStringLiteral("issues"), issues);
                               document.remove(QStringLiteral("disposition_plans"));
                               document.remove(QStringLiteral("authored_disposition_plan_id"));
                           }));
    QVERIFY(mutateResource(
        pack.path(), QStringLiteral("resources/workflow.json"), [](QJsonObject& document) {
            auto operations = document.value(QStringLiteral("operations")).toArray();
            for (qsizetype index = 0; index < operations.size(); ++index) {
                auto operation = operations.at(index).toObject();
                operation.remove(QStringLiteral("preconditions"));
                operations.replace(index, operation);
            }
            document.insert(QStringLiteral("operations"), operations);
        }));
    const auto declared_but_unused = PackReader::readDirectory(pack.path());
    QVERIFY2(declared_but_unused.has_value(),
             declared_but_unused ? "" : qPrintable(declared_but_unused.error().message));
    QVERIFY(mutateManifest(pack.path(), [](QJsonObject& manifest) {
        QJsonArray capabilities;
        for (const auto& value :
             manifest.value(QStringLiteral("required_capabilities")).toArray()) {
            const auto id = value.toObject().value(QStringLiteral("id")).toString();
            if (id != QStringLiteral("workbench.pack.structured-disposition") &&
                id != QStringLiteral("workbench.pack.workflow-preconditions")) {
                capabilities.push_back(value);
            }
        }
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    }));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& runtime_case = runtime->cases.front();
    QVERIFY(runtime_case.definition.disposition_targets.empty());
    QVERIFY(runtime_case.definition.disposition_plans.empty());
    QVERIFY(!runtime_case.definition.authored_disposition_plan_id.has_value());
    QVERIFY(!runtime_case.definition.authored_disposition_operation_id.has_value());
    QVERIFY(std::ranges::all_of(runtime_case.workflow.operations, [](const auto& operation) {
        return operation.preconditions.empty();
    }));
}

void SchemaDispatchTest::rejectsUnderdeclaredDispositionAndPreconditionCapabilities() {
    const std::array feature_capabilities{
        std::string("workbench.pack.structured-disposition"),
        std::string("workbench.pack.workflow-preconditions"),
        std::string("workbench.pack.grounded-questions"),
    };
    for (const auto& omitted : feature_capabilities) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateManifest(pack.path(), [&](QJsonObject& manifest) {
            QJsonArray capabilities;
            for (const auto& value :
                 manifest.value(QStringLiteral("required_capabilities")).toArray()) {
                if (value.toObject().value(QStringLiteral("id")).toString().toStdString() !=
                    omitted) {
                    capabilities.push_back(value);
                }
            }
            manifest.insert(QStringLiteral("required_capabilities"), capabilities);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::UnsupportedCapability);
    }

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    for (const auto& omitted : feature_capabilities) {
        auto forged = *loaded;
        std::erase_if(forged.required_capabilities,
                      [&](const auto& capability) { return capability.id == omitted; });
        const auto closure = PackReader::validateResolvedGraph(forged, {});
        QVERIFY(!closure.has_value());
        QCOMPARE(closure.error().code, ErrorCode::UnsupportedCapability);
        const auto runtime = appellate::packs::loadRuntimePack(forged);
        QVERIFY(!runtime.has_value());
        QCOMPARE(runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);
    }
}

void SchemaDispatchTest::validatesClosedWorkflowPreconditions() {
    const auto workflow_path = QStringLiteral("resources/workflow.json");
    const auto set_preconditions = [](QJsonObject& document, const QJsonArray& preconditions) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() ==
                QStringLiteral("example.operation.issue-judgment")) {
                operation.insert(QStringLiteral("preconditions"), preconditions);
                operations.replace(index, operation);
                break;
            }
        }
        document.insert(QStringLiteral("operations"), operations);
    };

    for (int variant = 0; variant < 3; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), workflow_path, [&](QJsonObject& document) {
            auto malformed = QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
                {QStringLiteral("present"), true},
            };
            if (variant == 0) {
                malformed.remove(QStringLiteral("present"));
            } else if (variant == 1) {
                malformed.insert(QStringLiteral("order_id"),
                                 QStringLiteral("example.order.dynamic"));
            }
            set_preconditions(document, variant == 2 ? QJsonArray{} : QJsonArray{malformed});
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
    }

    const std::array contradictions{
        QJsonArray{
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("filing_presence")},
                        {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
                        {QStringLiteral("present"), true}},
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("filing_presence")},
                        {QStringLiteral("filing_type_id"), QStringLiteral("example.filing.notice")},
                        {QStringLiteral("present"), false}},
        },
        QJsonArray{
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                        {QStringLiteral("status"), QStringLiteral("open")}},
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                        {QStringLiteral("status"), QStringLiteral("satisfied")}},
        },
        QJsonArray{
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                        {QStringLiteral("status"), QStringLiteral("elapsed")}},
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                        {QStringLiteral("status"), QStringLiteral("not_elapsed")}},
        },
    };
    for (const auto& preconditions : contradictions) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), workflow_path, [&](QJsonObject& document) {
            set_preconditions(document, preconditions);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir unresolved_filing;
    QVERIFY(unresolved_filing.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unresolved_filing.path()));
    QVERIFY(mutateResource(unresolved_filing.path(), workflow_path, [&](QJsonObject& document) {
        set_preconditions(document, QJsonArray{QJsonObject{
                                        {QStringLiteral("kind"), QStringLiteral("filing_presence")},
                                        {QStringLiteral("filing_type_id"),
                                         QStringLiteral("example.filing.missing")},
                                        {QStringLiteral("present"), true},
                                    }});
    }));
    const auto unresolved = PackReader::readDirectory(unresolved_filing.path());
    QVERIFY(!unresolved.has_value());
    QCOMPARE(unresolved.error().code, ErrorCode::CrossReferenceFailure);

    QTemporaryDir orthogonal;
    QVERIFY(orthogonal.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), orthogonal.path()));
    QVERIFY(mutateResource(orthogonal.path(), workflow_path, [&](QJsonObject& document) {
        auto routes = document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.first().toObject();
        route.insert(
            QStringLiteral("accepted_deadline"),
            QJsonObject{{QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                        {QStringLiteral("operation_id"),
                         QStringLiteral("example.operation.calculate-cure")}});
        routes.replace(0, route);
        document.insert(QStringLiteral("filing_routes"), routes);
        set_preconditions(
            document,
            QJsonArray{
                QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                    {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                    {QStringLiteral("status"), QStringLiteral("open")}},
                QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                    {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.dynamic")},
                    {QStringLiteral("status"), QStringLiteral("elapsed")}},
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("order_disposition")},
                            {QStringLiteral("order_id"), QStringLiteral("example.order.dynamic")},
                            {QStringLiteral("disposition"), QStringLiteral("granted")}},
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("argument_scheduled")},
                            {QStringLiteral("scheduled"), false}},
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("judgment_issued")},
                            {QStringLiteral("issued"), true}},
            });
    }));
    const auto orthogonal_loaded = PackReader::readDirectory(orthogonal.path());
    QVERIFY2(orthogonal_loaded.has_value(),
             orthogonal_loaded ? "" : qPrintable(orthogonal_loaded.error().message));
    const auto orthogonal_runtime = appellate::packs::loadRuntimePack(*orthogonal_loaded);
    QVERIFY2(orthogonal_runtime.has_value(),
             orthogonal_runtime ? "" : orthogonal_runtime.error().message.c_str());
    const auto judgment =
        std::ranges::find(orthogonal_runtime->cases.front().workflow.operations,
                          appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
                          &appellate::model::WorkflowOperation::id);
    QVERIFY(judgment != orthogonal_runtime->cases.front().workflow.operations.end());
    QCOMPARE(judgment->preconditions.size(), std::size_t{5});
    const auto* open =
        std::get_if<appellate::model::WorkflowDeadlinePrecondition>(&judgment->preconditions.at(0));
    const auto* elapsed =
        std::get_if<appellate::model::WorkflowDeadlinePrecondition>(&judgment->preconditions.at(1));
    const auto* order =
        std::get_if<appellate::model::WorkflowOrderPrecondition>(&judgment->preconditions.at(2));
    const auto* argument =
        std::get_if<appellate::model::WorkflowArgumentPrecondition>(&judgment->preconditions.at(3));
    const auto* issued =
        std::get_if<appellate::model::WorkflowJudgmentPrecondition>(&judgment->preconditions.at(4));
    QVERIFY(open != nullptr && elapsed != nullptr && order != nullptr && argument != nullptr &&
            issued != nullptr);
    QCOMPARE(open->condition, appellate::model::WorkflowDeadlineCondition::Open);
    QCOMPARE(elapsed->condition, appellate::model::WorkflowDeadlineCondition::Elapsed);
    QCOMPARE(order->disposition, appellate::model::WorkflowOrderDisposition::Granted);
    QVERIFY(!argument->scheduled);
    QVERIFY(issued->issued);

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto workflow = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
    });
    QVERIFY(workflow != forged.resources.end());
    set_preconditions(workflow->document, contradictions.front());
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);
}

void SchemaDispatchTest::validatesDependentDeadlineBasesAndReachedCondition() {
    const auto workflow_path = QStringLiteral("resources/workflow.json");
    const auto declare_capability = [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        capabilities.push_back(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("workbench.pack.dependent-deadlines")},
            {QStringLiteral("version"), 1}});
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    };
    const auto declare_named_capability = [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        capabilities.push_back(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("workbench.pack.named-deadlines")},
                        {QStringLiteral("version"), 1}});
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    };
    const auto mutate_operation = [](QJsonObject& document, const QString& operation_id,
                                     const std::function<void(QJsonObject&)>& mutation) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() == operation_id) {
                mutation(operation);
                operations.replace(index, operation);
                break;
            }
        }
        document.insert(QStringLiteral("operations"), operations);
    };
    const auto add_dependent_operation = [](QJsonObject& document,
                                            const std::function<void(QJsonObject&)>& mutation) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() ==
                QStringLiteral("example.operation.calculate-cure")) {
                auto prior = operation;
                prior.insert(QStringLiteral("operation_id"),
                             QStringLiteral("example.operation.calculate-prior"));
                prior.insert(QStringLiteral("produced_deadline_id"),
                             QStringLiteral("example.deadline.prior"));
                operations.push_back(prior);
                operation.insert(QStringLiteral("operation_id"),
                                 QStringLiteral("example.operation.calculate-dependent"));
                operation.insert(QStringLiteral("produced_deadline_id"),
                                 QStringLiteral("example.deadline.dependent"));
                mutation(operation);
                operations.push_back(operation);
                break;
            }
        }
        document.insert(QStringLiteral("operations"), operations);
    };
    const auto bind_dependent_to_route = [](QJsonObject& document, bool deficiency) {
        auto routes = document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.first().toObject();
        const QJsonObject plan{
            {QStringLiteral("deadline_id"),
             deficiency ? QStringLiteral("example.deadline.notice-cure")
                        : QStringLiteral("example.deadline.after-acceptance")},
            {QStringLiteral("operation_id"),
             QStringLiteral("example.operation.calculate-dependent")},
        };
        route.insert(deficiency ? QStringLiteral("deficiency_deadline")
                                : QStringLiteral("accepted_deadline"),
                     plan);
        routes.replace(0, route);
        document.insert(QStringLiteral("filing_routes"), routes);
    };
    const auto authorize_route_deadline = [&](QJsonObject& document) {
        mutate_operation(document, QStringLiteral("example.operation.calculate-cure"),
                         [](QJsonObject& operation) {
                             operation.insert(QStringLiteral("authorized_role_ids"),
                                              QJsonArray{QStringLiteral("example.role.court")});
                         });
    };
    const auto overlap_named_with_deficiency = [&](QJsonObject& document) {
        const auto collision = QStringLiteral("example.deadline.notice-cure.command.future");
        mutate_operation(document, QStringLiteral("example.operation.calculate-prior"),
                         [&](QJsonObject& operation) {
                             operation.insert(QStringLiteral("produced_deadline_id"), collision);
                         });
        mutate_operation(document, QStringLiteral("example.operation.calculate-dependent"),
                         [&](QJsonObject& operation) {
                             operation.insert(QStringLiteral("deadline_base_id"), collision);
                             auto preconditions =
                                 operation.value(QStringLiteral("preconditions")).toArray();
                             for (qsizetype index = 0; index < preconditions.size(); ++index) {
                                 auto precondition = preconditions.at(index).toObject();
                                 precondition.insert(QStringLiteral("deadline_id"), collision);
                                 preconditions.replace(index, precondition);
                             }
                             operation.insert(QStringLiteral("preconditions"), preconditions);
                         });
    };
    const auto add_nested_deficiency_route = [](QJsonObject& document) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        const auto clone_operation = [&](const QString& source_id, const QString& target_id) {
            for (qsizetype index = 0; index < operations.size(); ++index) {
                auto operation = operations.at(index).toObject();
                if (operation.value(QStringLiteral("operation_id")).toString() != source_id) {
                    continue;
                }
                operation.insert(QStringLiteral("operation_id"), target_id);
                operation.insert(QStringLiteral("stage_id"),
                                 QStringLiteral("example.stage.submitted"));
                operations.push_back(operation);
                return;
            }
        };
        clone_operation(QStringLiteral("example.operation.accept-notice"),
                        QStringLiteral("example.operation.accept-notice-submitted"));
        clone_operation(QStringLiteral("example.operation.issue-deficiency"),
                        QStringLiteral("example.operation.issue-deficiency-submitted"));
        clone_operation(QStringLiteral("example.operation.calculate-cure"),
                        QStringLiteral("example.operation.calculate-cure-submitted"));
        document.insert(QStringLiteral("operations"), operations);

        auto routes = document.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.first().toObject();
        route.insert(QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted"));
        route.insert(QStringLiteral("accept_operation_id"),
                     QStringLiteral("example.operation.accept-notice-submitted"));
        route.insert(QStringLiteral("reject_operation_id"),
                     QStringLiteral("example.operation.reject-submitted"));
        route.insert(QStringLiteral("deficiency_operation_id"),
                     QStringLiteral("example.operation.issue-deficiency-submitted"));
        route.insert(QStringLiteral("deficiency_deadline"),
                     QJsonObject{
                         {QStringLiteral("deadline_id"),
                          QStringLiteral("example.deadline.notice-cure.child")},
                         {QStringLiteral("operation_id"),
                          QStringLiteral("example.operation.calculate-cure-submitted")},
                     });
        route.remove(QStringLiteral("accepted_deadline"));
        route.remove(QStringLiteral("advance_operation_id"));
        routes.push_back(route);
        document.insert(QStringLiteral("filing_routes"), routes);
    };

    QTemporaryDir valid;
    QVERIFY(valid.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), valid.path()));
    QVERIFY(mutateManifest(valid.path(), declare_capability));
    QVERIFY(mutateManifest(valid.path(), declare_named_capability));
    QVERIFY(mutateResource(valid.path(), workflow_path, [&](QJsonObject& document) {
        add_dependent_operation(document, [](QJsonObject& operation) {
            operation.insert(QStringLiteral("deadline_base_id"),
                             QStringLiteral("example.deadline.prior"));
            operation.insert(
                QStringLiteral("preconditions"),
                QJsonArray{
                    QJsonObject{
                        {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.prior")},
                        {QStringLiteral("status"), QStringLiteral("reached")},
                    },
                    QJsonObject{
                        {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.prior")},
                        {QStringLiteral("status"), QStringLiteral("not_elapsed")},
                    },
                });
        });
    }));
    const auto loaded = PackReader::readDirectory(valid.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto parsed_operation = std::ranges::find(
        runtime->cases.front().workflow.operations,
        appellate::model::WorkflowOperationId{"example.operation.calculate-dependent"},
        &appellate::model::WorkflowOperation::id);
    QVERIFY(parsed_operation != runtime->cases.front().workflow.operations.end());
    QCOMPARE(parsed_operation->deadline_base_id,
             std::optional{appellate::model::WorkflowDeadlineId{"example.deadline.prior"}});
    QCOMPARE(parsed_operation->produced_deadline_id,
             std::optional{appellate::model::WorkflowDeadlineId{"example.deadline.dependent"}});
    QCOMPARE(parsed_operation->preconditions.size(), std::size_t{2});
    const auto* reached = std::get_if<appellate::model::WorkflowDeadlinePrecondition>(
        &parsed_operation->preconditions[0]);
    QVERIFY(reached != nullptr);
    QCOMPARE(reached->condition, appellate::model::WorkflowDeadlineCondition::Reached);
    const auto* not_elapsed = std::get_if<appellate::model::WorkflowDeadlinePrecondition>(
        &parsed_operation->preconditions[1]);
    QVERIFY(not_elapsed != nullptr);
    QCOMPARE(not_elapsed->condition, appellate::model::WorkflowDeadlineCondition::NotElapsed);

    QTemporaryDir missing_capability;
    QVERIFY(missing_capability.isValid());
    QVERIFY(copyTree(valid.path(), missing_capability.path()));
    QVERIFY(mutateManifest(missing_capability.path(), [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        for (qsizetype index = capabilities.size(); index > 0; --index) {
            if (capabilities.at(index - 1).toObject().value(QStringLiteral("id")).toString() ==
                QStringLiteral("workbench.pack.dependent-deadlines")) {
                capabilities.removeAt(index - 1);
            }
        }
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    }));
    const auto missing_capability_result = PackReader::readDirectory(missing_capability.path());
    QVERIFY(!missing_capability_result.has_value());
    QCOMPARE(missing_capability_result.error().code, ErrorCode::UnsupportedCapability);

    QTemporaryDir missing_named_capability;
    QVERIFY(missing_named_capability.isValid());
    QVERIFY(copyTree(valid.path(), missing_named_capability.path()));
    QVERIFY(mutateManifest(missing_named_capability.path(), [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        for (qsizetype index = capabilities.size(); index > 0; --index) {
            if (capabilities.at(index - 1).toObject().value(QStringLiteral("id")).toString() ==
                QStringLiteral("workbench.pack.named-deadlines")) {
                capabilities.removeAt(index - 1);
            }
        }
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    }));
    const auto missing_named_result = PackReader::readDirectory(missing_named_capability.path());
    QVERIFY(!missing_named_result.has_value());
    QCOMPARE(missing_named_result.error().code, ErrorCode::UnsupportedCapability);

    QTemporaryDir base_only_without_capability;
    QVERIFY(base_only_without_capability.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")),
                     base_only_without_capability.path()));
    QVERIFY(mutateManifest(base_only_without_capability.path(), declare_named_capability));
    QVERIFY(mutateResource(base_only_without_capability.path(), workflow_path,
                           [&](QJsonObject& document) {
                               add_dependent_operation(document, [](QJsonObject& operation) {
                                   operation.insert(QStringLiteral("deadline_base_id"),
                                                    QStringLiteral("example.deadline.prior"));
                               });
                           }));
    const auto base_only_result = PackReader::readDirectory(base_only_without_capability.path());
    QVERIFY(!base_only_result.has_value());
    QCOMPARE(base_only_result.error().code, ErrorCode::UnsupportedCapability);

    QTemporaryDir reached_only_without_capability;
    QVERIFY(reached_only_without_capability.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")),
                     reached_only_without_capability.path()));
    QVERIFY(mutateManifest(reached_only_without_capability.path(), declare_named_capability));
    QVERIFY(mutateResource(
        reached_only_without_capability.path(), workflow_path, [&](QJsonObject& document) {
            add_dependent_operation(document, [](QJsonObject& operation) {
                operation.insert(
                    QStringLiteral("preconditions"),
                    QJsonArray{QJsonObject{
                        {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                        {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.prior")},
                        {QStringLiteral("status"), QStringLiteral("reached")},
                    }});
            });
        }));
    const auto reached_only_result =
        PackReader::readDirectory(reached_only_without_capability.path());
    QVERIFY(!reached_only_result.has_value());
    QCOMPARE(reached_only_result.error().code, ErrorCode::UnsupportedCapability);

    for (const auto& capability_id :
         {"workbench.pack.dependent-deadlines", "workbench.pack.named-deadlines"}) {
        auto forged_missing_capability = *loaded;
        std::erase_if(forged_missing_capability.required_capabilities,
                      [&](const auto& capability) { return capability.id == capability_id; });
        const auto forged_missing_graph =
            PackReader::validateResolvedGraph(forged_missing_capability, {});
        QVERIFY(!forged_missing_graph.has_value());
        QCOMPARE(forged_missing_graph.error().code, ErrorCode::UnsupportedCapability);
        const auto forged_missing_runtime =
            appellate::packs::loadRuntimePack(forged_missing_capability);
        QVERIFY(!forged_missing_runtime.has_value());
        QCOMPARE(forged_missing_runtime.error().code,
                 appellate::packs::RuntimePackErrorCode::InvalidPack);
    }

    QTemporaryDir declared_unused;
    QVERIFY(declared_unused.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), declared_unused.path()));
    QVERIFY(mutateManifest(declared_unused.path(), declare_capability));
    QVERIFY(mutateManifest(declared_unused.path(), declare_named_capability));
    const auto declared_unused_result = PackReader::readDirectory(declared_unused.path());
    QVERIFY2(declared_unused_result.has_value(),
             declared_unused_result ? "" : qPrintable(declared_unused_result.error().message));

    for (const bool deficiency : {false, true}) {
        QTemporaryDir route_bound;
        QVERIFY(route_bound.isValid());
        QVERIFY(copyTree(valid.path(), route_bound.path()));
        QVERIFY(mutateResource(route_bound.path(), workflow_path, [&](QJsonObject& document) {
            bind_dependent_to_route(document, deficiency);
        }));
        const auto route_bound_result = PackReader::readDirectory(route_bound.path());
        QVERIFY(!route_bound_result.has_value());
        QCOMPARE(route_bound_result.error().code, ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir authorized_route;
    QVERIFY(authorized_route.isValid());
    QVERIFY(copyTree(valid.path(), authorized_route.path()));
    QVERIFY(mutateResource(authorized_route.path(), workflow_path, authorize_route_deadline));
    const auto authorized_route_result = PackReader::readDirectory(authorized_route.path());
    QVERIFY2(authorized_route_result.has_value(),
             authorized_route_result ? "" : qPrintable(authorized_route_result.error().message));
    const auto authorized_route_runtime =
        appellate::packs::loadRuntimePack(*authorized_route_result);
    QVERIFY2(authorized_route_runtime.has_value(),
             authorized_route_runtime ? "" : authorized_route_runtime.error().message.c_str());

    QTemporaryDir exact_namespace_overlap;
    QVERIFY(exact_namespace_overlap.isValid());
    QVERIFY(copyTree(valid.path(), exact_namespace_overlap.path()));
    QVERIFY(mutateResource(exact_namespace_overlap.path(), workflow_path,
                           overlap_named_with_deficiency));
    const auto exact_namespace_result = PackReader::readDirectory(exact_namespace_overlap.path());
    QVERIFY(!exact_namespace_result.has_value());
    QCOMPARE(exact_namespace_result.error().code, ErrorCode::CrossReferenceFailure);

    QTemporaryDir nested_namespace_overlap;
    QVERIFY(nested_namespace_overlap.isValid());
    QVERIFY(copyTree(valid.path(), nested_namespace_overlap.path()));
    QVERIFY(mutateResource(nested_namespace_overlap.path(), workflow_path,
                           add_nested_deficiency_route));
    const auto nested_namespace_result = PackReader::readDirectory(nested_namespace_overlap.path());
    QVERIFY(!nested_namespace_result.has_value());
    QCOMPARE(nested_namespace_result.error().code, ErrorCode::CrossReferenceFailure);

    QTemporaryDir wrong_opcode;
    QVERIFY(wrong_opcode.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), wrong_opcode.path()));
    QVERIFY(mutateManifest(wrong_opcode.path(), declare_capability));
    QVERIFY(mutateResource(wrong_opcode.path(), workflow_path, [&](QJsonObject& document) {
        mutate_operation(document, QStringLiteral("example.operation.issue-judgment"),
                         [](QJsonObject& operation) {
                             operation.insert(QStringLiteral("deadline_base_id"),
                                              QStringLiteral("example.deadline.prior"));
                         });
    }));
    const auto wrong_opcode_result = PackReader::readDirectory(wrong_opcode.path());
    QVERIFY(!wrong_opcode_result.has_value());
    QCOMPARE(wrong_opcode_result.error().code, ErrorCode::CrossReferenceFailure);

    QTemporaryDir malformed;
    QVERIFY(malformed.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), malformed.path()));
    QVERIFY(mutateManifest(malformed.path(), declare_capability));
    QVERIFY(mutateManifest(malformed.path(), declare_named_capability));
    QVERIFY(mutateResource(malformed.path(), workflow_path, [&](QJsonObject& document) {
        add_dependent_operation(document, [](QJsonObject& operation) {
            operation.insert(QStringLiteral("deadline_base_id"), QStringLiteral("not_namespaced"));
        });
    }));
    const auto malformed_result = PackReader::readDirectory(malformed.path());
    QVERIFY(!malformed_result.has_value());
    QCOMPARE(malformed_result.error().code, ErrorCode::SchemaViolation);

    QTemporaryDir v1;
    QVERIFY(v1.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), v1.path()));
    QVERIFY(mutateResource(v1.path(), workflow_path, [&](QJsonObject& document) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        auto operation = operations.first().toObject();
        operation.insert(QStringLiteral("deadline_base_id"),
                         QStringLiteral("example.deadline.prior"));
        operations.replace(0, operation);
        document.insert(QStringLiteral("operations"), operations);
    }));
    const auto v1_result = PackReader::readDirectory(v1.path());
    QVERIFY(!v1_result.has_value());
    QCOMPARE(v1_result.error().code, ErrorCode::SchemaViolation);

    QTemporaryDir v1_declaration;
    QVERIFY(v1_declaration.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), v1_declaration.path()));
    QVERIFY(mutateManifest(v1_declaration.path(), declare_capability));
    const auto v1_declaration_result = PackReader::readDirectory(v1_declaration.path());
    QVERIFY(!v1_declaration_result.has_value());
    QCOMPARE(v1_declaration_result.error().code, ErrorCode::UnsupportedCapability);

    auto forged = *loaded;
    const auto forged_workflow = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
    });
    QVERIFY(forged_workflow != forged.resources.end());
    mutate_operation(forged_workflow->document, QStringLiteral("example.operation.issue-judgment"),
                     [](QJsonObject& forged_operation) {
                         forged_operation.insert(QStringLiteral("deadline_base_id"),
                                                 QStringLiteral("example.deadline.prior"));
                     });
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);

    for (const bool deficiency : {false, true}) {
        auto forged_route = *loaded;
        const auto forged_route_workflow =
            std::ranges::find_if(forged_route.resources, [](const auto& resource) {
                return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
            });
        QVERIFY(forged_route_workflow != forged_route.resources.end());
        bind_dependent_to_route(forged_route_workflow->document, deficiency);
        const auto forged_route_graph = PackReader::validateResolvedGraph(forged_route, {});
        QVERIFY(!forged_route_graph.has_value());
        QCOMPARE(forged_route_graph.error().code, ErrorCode::CrossReferenceFailure);
        const auto forged_route_runtime = appellate::packs::loadRuntimePack(forged_route);
        QVERIFY(!forged_route_runtime.has_value());
        QCOMPARE(forged_route_runtime.error().code,
                 appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
    }

    auto forged_authorized_route = *loaded;
    const auto forged_authorized_workflow =
        std::ranges::find_if(forged_authorized_route.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
        });
    QVERIFY(forged_authorized_workflow != forged_authorized_route.resources.end());
    authorize_route_deadline(forged_authorized_workflow->document);
    const auto forged_authorized_graph =
        PackReader::validateResolvedGraph(forged_authorized_route, {});
    QVERIFY2(forged_authorized_graph.has_value(),
             forged_authorized_graph ? "" : qPrintable(forged_authorized_graph.error().message));
    const auto forged_authorized_runtime =
        appellate::packs::loadRuntimePack(forged_authorized_route);
    QVERIFY2(forged_authorized_runtime.has_value(),
             forged_authorized_runtime ? "" : forged_authorized_runtime.error().message.c_str());

    const std::array<std::function<void(QJsonObject&)>, 2> namespace_mutations{
        overlap_named_with_deficiency,
        add_nested_deficiency_route,
    };
    for (const auto& mutation : namespace_mutations) {
        auto forged_namespace = *loaded;
        const auto forged_namespace_workflow =
            std::ranges::find_if(forged_namespace.resources, [](const auto& resource) {
                return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
            });
        QVERIFY(forged_namespace_workflow != forged_namespace.resources.end());
        mutation(forged_namespace_workflow->document);
        const auto forged_namespace_graph = PackReader::validateResolvedGraph(forged_namespace, {});
        QVERIFY(!forged_namespace_graph.has_value());
        QCOMPARE(forged_namespace_graph.error().code, ErrorCode::CrossReferenceFailure);
        const auto forged_namespace_runtime = appellate::packs::loadRuntimePack(forged_namespace);
        QVERIFY(!forged_namespace_runtime.has_value());
        QCOMPARE(forged_namespace_runtime.error().code,
                 appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir unnamed_extended;
    QVERIFY(unnamed_extended.isValid());
    QVERIFY(copyTree(valid.path(), unnamed_extended.path()));
    QVERIFY(mutateResource(unnamed_extended.path(), workflow_path, [&](QJsonObject& document) {
        mutate_operation(document, QStringLiteral("example.operation.calculate-dependent"),
                         [](QJsonObject& operation) {
                             operation.remove(QStringLiteral("deadline_base_id"));
                             operation.remove(QStringLiteral("produced_deadline_id"));
                         });
    }));
    const auto unnamed_extended_result = PackReader::readDirectory(unnamed_extended.path());
    QVERIFY(!unnamed_extended_result.has_value());
    QCOMPARE(unnamed_extended_result.error().code, ErrorCode::CrossReferenceFailure);

    auto forged_unnamed_extended = *loaded;
    const auto forged_unnamed_workflow =
        std::ranges::find_if(forged_unnamed_extended.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
        });
    QVERIFY(forged_unnamed_workflow != forged_unnamed_extended.resources.end());
    mutate_operation(forged_unnamed_workflow->document,
                     QStringLiteral("example.operation.calculate-dependent"),
                     [](QJsonObject& operation) {
                         operation.remove(QStringLiteral("deadline_base_id"));
                         operation.remove(QStringLiteral("produced_deadline_id"));
                     });
    const auto forged_unnamed_graph =
        PackReader::validateResolvedGraph(forged_unnamed_extended, {});
    QVERIFY(!forged_unnamed_graph.has_value());
    QCOMPARE(forged_unnamed_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_unnamed_runtime = appellate::packs::loadRuntimePack(forged_unnamed_extended);
    QVERIFY(!forged_unnamed_runtime.has_value());
    QCOMPARE(forged_unnamed_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::validatesNamedEventDateAndArgumentDateFeatures() {
    const auto workflow_path = QStringLiteral("resources/workflow.json");
    const auto declare = [](QJsonObject& manifest, const QString& id) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        capabilities.push_back(
            QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("version"), 1}});
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    };
    const auto add_capabilities = [&](QJsonObject& manifest) {
        declare(manifest, QStringLiteral("workbench.pack.named-deadlines"));
        declare(manifest, QStringLiteral("workbench.pack.event-date-deadlines"));
        declare(manifest, QStringLiteral("workbench.pack.argument-date-guards"));
    };
    const auto mutate_operation = [](QJsonObject& document, const QString& operation_id,
                                     const std::function<void(QJsonObject&)>& mutation) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() == operation_id) {
                mutation(operation);
                operations.replace(index, operation);
                break;
            }
        }
        document.insert(QStringLiteral("operations"), operations);
    };
    const auto add_features = [](QJsonObject& document) {
        auto operations = document.value(QStringLiteral("operations")).toArray();
        QJsonObject deadline_template;
        QJsonObject court_template;
        for (const auto& value : operations) {
            const auto operation = value.toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() ==
                QStringLiteral("example.operation.calculate-cure")) {
                deadline_template = operation;
            } else if (operation.value(QStringLiteral("operation_id")).toString() ==
                       QStringLiteral("example.operation.issue-judgment")) {
                court_template = operation;
            }
        }

        auto named = deadline_template;
        named.insert(QStringLiteral("operation_id"),
                     QStringLiteral("example.operation.calculate-named"));
        named.insert(QStringLiteral("produced_deadline_id"),
                     QStringLiteral("example.deadline.named"));
        operations.push_back(named);

        auto source_order = court_template;
        source_order.insert(QStringLiteral("operation_id"),
                            QStringLiteral("example.operation.enter-clock-order"));
        source_order.insert(QStringLiteral("opcode"), QStringLiteral("enter_order"));
        source_order.remove(QStringLiteral("preconditions"));
        operations.push_back(source_order);

        auto schedule = court_template;
        schedule.insert(QStringLiteral("operation_id"),
                        QStringLiteral("example.operation.schedule-argument"));
        schedule.insert(QStringLiteral("opcode"), QStringLiteral("schedule_argument"));
        schedule.remove(QStringLiteral("preconditions"));
        operations.push_back(schedule);

        auto judgment_clock = deadline_template;
        judgment_clock.insert(QStringLiteral("operation_id"),
                              QStringLiteral("example.operation.calculate-judgment-clock"));
        judgment_clock.insert(QStringLiteral("stage_id"),
                              QStringLiteral("example.stage.submitted"));
        judgment_clock.insert(QStringLiteral("produced_deadline_id"),
                              QStringLiteral("example.deadline.judgment-clock"));
        judgment_clock.insert(
            QStringLiteral("deadline_event_base"),
            QJsonObject{{QStringLiteral("kind"), QStringLiteral("judgment_occurred")}});
        judgment_clock.insert(QStringLiteral("authorized_role_ids"),
                              QJsonArray{QStringLiteral("example.role.court")});
        operations.push_back(judgment_clock);

        auto order_clock = deadline_template;
        order_clock.insert(QStringLiteral("operation_id"),
                           QStringLiteral("example.operation.calculate-order-clock"));
        order_clock.insert(QStringLiteral("stage_id"), QStringLiteral("example.stage.submitted"));
        order_clock.insert(QStringLiteral("produced_deadline_id"),
                           QStringLiteral("example.deadline.order-clock"));
        order_clock.insert(
            QStringLiteral("deadline_event_base"),
            QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("order_occurred")},
                {QStringLiteral("order_id"), QStringLiteral("example.order.clock-source")},
                {QStringLiteral("operation_id"),
                 QStringLiteral("example.operation.enter-clock-order")},
            });
        order_clock.insert(QStringLiteral("authorized_role_ids"),
                           QJsonArray{QStringLiteral("example.role.court")});
        operations.push_back(order_clock);

        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() !=
                QStringLiteral("example.operation.issue-judgment")) {
                continue;
            }
            auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
            preconditions.push_back(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.named")},
                {QStringLiteral("status"), QStringLiteral("open")},
            });
            preconditions.push_back(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("argument_scheduled")},
                {QStringLiteral("scheduled"), true},
            });
            preconditions.push_back(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("argument_date_status")},
                {QStringLiteral("status"), QStringLiteral("reached")},
            });
            operation.insert(QStringLiteral("preconditions"), preconditions);
            operations.replace(index, operation);
            break;
        }
        document.insert(QStringLiteral("operations"), operations);
    };

    QTemporaryDir valid;
    QVERIFY(valid.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), valid.path()));
    QVERIFY(mutateManifest(valid.path(), add_capabilities));
    QVERIFY(mutateResource(valid.path(), workflow_path, add_features));
    const auto loaded = PackReader::readDirectory(valid.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto runtime = appellate::packs::loadRuntimePack(*loaded);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    const auto& operations = runtime->cases.front().workflow.operations;
    const auto judgment_clock = std::ranges::find(
        operations,
        appellate::model::WorkflowOperationId{"example.operation.calculate-judgment-clock"},
        &appellate::model::WorkflowOperation::id);
    QVERIFY(judgment_clock != operations.end());
    QVERIFY(std::holds_alternative<appellate::model::WorkflowJudgmentOccurredDeadlineBase>(
        *judgment_clock->deadline_event_base));
    const auto order_clock = std::ranges::find(
        operations,
        appellate::model::WorkflowOperationId{"example.operation.calculate-order-clock"},
        &appellate::model::WorkflowOperation::id);
    QVERIFY(order_clock != operations.end());
    const auto* order_base = std::get_if<appellate::model::WorkflowOrderOccurredDeadlineBase>(
        &*order_clock->deadline_event_base);
    QVERIFY(order_base != nullptr);
    QCOMPARE(order_base->order_id, appellate::model::WorkflowOrderId{"example.order.clock-source"});
    QCOMPARE(order_base->operation_id,
             appellate::model::WorkflowOperationId{"example.operation.enter-clock-order"});
    const auto judgment = std::ranges::find(
        operations, appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
        &appellate::model::WorkflowOperation::id);
    QVERIFY(judgment != operations.end());
    QVERIFY(std::ranges::any_of(judgment->preconditions, [](const auto& precondition) {
        return std::holds_alternative<appellate::model::WorkflowArgumentDatePrecondition>(
            precondition);
    }));

    for (const auto& capability_id : {QStringLiteral("workbench.pack.named-deadlines"),
                                      QStringLiteral("workbench.pack.event-date-deadlines"),
                                      QStringLiteral("workbench.pack.argument-date-guards")}) {
        QTemporaryDir missing;
        QVERIFY(missing.isValid());
        QVERIFY(copyTree(valid.path(), missing.path()));
        QVERIFY(mutateManifest(missing.path(), [&](QJsonObject& manifest) {
            auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
            for (qsizetype index = capabilities.size(); index > 0; --index) {
                if (capabilities.at(index - 1).toObject().value(QStringLiteral("id")).toString() ==
                    capability_id) {
                    capabilities.removeAt(index - 1);
                }
            }
            manifest.insert(QStringLiteral("required_capabilities"), capabilities);
        }));
        const auto missing_result = PackReader::readDirectory(missing.path());
        QVERIFY(!missing_result.has_value());
        QCOMPARE(missing_result.error().code, ErrorCode::UnsupportedCapability);

        auto forged_missing = *loaded;
        std::erase_if(forged_missing.required_capabilities, [&](const auto& capability) {
            return capability.id == capability_id.toStdString();
        });
        const auto forged_graph = PackReader::validateResolvedGraph(forged_missing, {});
        QVERIFY(!forged_graph.has_value());
        QCOMPARE(forged_graph.error().code, ErrorCode::UnsupportedCapability);
        const auto forged_runtime = appellate::packs::loadRuntimePack(forged_missing);
        QVERIFY(!forged_runtime.has_value());
        QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);
    }

    QTemporaryDir declared_unused;
    QVERIFY(declared_unused.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), declared_unused.path()));
    QVERIFY(mutateManifest(declared_unused.path(), add_capabilities));
    const auto unused_result = PackReader::readDirectory(declared_unused.path());
    QVERIFY2(unused_result.has_value(),
             unused_result ? "" : qPrintable(unused_result.error().message));

    QTemporaryDir unknown_precondition;
    QVERIFY(unknown_precondition.isValid());
    QVERIFY(copyTree(valid.path(), unknown_precondition.path()));
    QVERIFY(mutateResource(unknown_precondition.path(), workflow_path, [&](QJsonObject& document) {
        mutate_operation(document, QStringLiteral("example.operation.issue-judgment"),
                         [](QJsonObject& operation) {
                             auto preconditions =
                                 operation.value(QStringLiteral("preconditions")).toArray();
                             auto deadline = preconditions.at(1).toObject();
                             deadline.insert(QStringLiteral("deadline_id"),
                                             QStringLiteral("example.deadline.unknown"));
                             preconditions.replace(1, deadline);
                             operation.insert(QStringLiteral("preconditions"), preconditions);
                         });
    }));
    const auto unknown_result = PackReader::readDirectory(unknown_precondition.path());
    QVERIFY(!unknown_result.has_value());
    QCOMPARE(unknown_result.error().code, ErrorCode::CrossReferenceFailure);

    auto forged_unknown = *loaded;
    const auto forged_unknown_workflow =
        std::ranges::find_if(forged_unknown.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Workflow;
        });
    QVERIFY(forged_unknown_workflow != forged_unknown.resources.end());
    mutate_operation(
        forged_unknown_workflow->document, QStringLiteral("example.operation.issue-judgment"),
        [](QJsonObject& operation) {
            auto preconditions = operation.value(QStringLiteral("preconditions")).toArray();
            auto deadline = preconditions.at(1).toObject();
            deadline.insert(QStringLiteral("deadline_id"),
                            QStringLiteral("example.deadline.unknown"));
            preconditions.replace(1, deadline);
            operation.insert(QStringLiteral("preconditions"), preconditions);
        });
    const auto forged_unknown_graph = PackReader::validateResolvedGraph(forged_unknown, {});
    QVERIFY(!forged_unknown_graph.has_value());
    QCOMPARE(forged_unknown_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_unknown_runtime = appellate::packs::loadRuntimePack(forged_unknown);
    QVERIFY(!forged_unknown_runtime.has_value());
    QCOMPARE(forged_unknown_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::CrossReferenceFailure);

    for (const bool remove_output : {false, true}) {
        QTemporaryDir invalid_event_base;
        QVERIFY(invalid_event_base.isValid());
        QVERIFY(copyTree(valid.path(), invalid_event_base.path()));
        QVERIFY(
            mutateResource(invalid_event_base.path(), workflow_path, [&](QJsonObject& document) {
                mutate_operation(
                    document, QStringLiteral("example.operation.calculate-order-clock"),
                    [&](QJsonObject& operation) {
                        if (remove_output) {
                            operation.remove(QStringLiteral("produced_deadline_id"));
                        } else {
                            auto event_base =
                                operation.value(QStringLiteral("deadline_event_base")).toObject();
                            event_base.insert(QStringLiteral("operation_id"),
                                              QStringLiteral("example.operation.issue-judgment"));
                            operation.insert(QStringLiteral("deadline_event_base"), event_base);
                        }
                    });
            }));
        const auto invalid_result = PackReader::readDirectory(invalid_event_base.path());
        QVERIFY(!invalid_result.has_value());
        QCOMPARE(invalid_result.error().code, ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir named_collision;
    QVERIFY(named_collision.isValid());
    QVERIFY(copyTree(valid.path(), named_collision.path()));
    QVERIFY(mutateResource(named_collision.path(), workflow_path, [&](QJsonObject& document) {
        mutate_operation(document, QStringLiteral("example.operation.calculate-named"),
                         [](QJsonObject& operation) {
                             operation.insert(QStringLiteral("produced_deadline_id"),
                                              QStringLiteral("example.deadline.notice-cure"));
                         });
    }));
    const auto collision_result = PackReader::readDirectory(named_collision.path());
    QVERIFY(!collision_result.has_value());
    QCOMPARE(collision_result.error().code, ErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::validatesStructuredDispositionPlans() {
    const auto case_path = QStringLiteral("resources/case.json");
    const QJsonObject fixed_vector_case{
        {QStringLiteral("resource_id"), QStringLiteral("example.case.fictional")},
        {QStringLiteral("authored_disposition_id"),
         QStringLiteral("example.operation.issue-judgment")},
    };
    const QJsonObject fixed_vector_plan{
        {QStringLiteral("plan_id"), QStringLiteral("example.disposition.fictional")},
        {QStringLiteral("finality"), QStringLiteral("final")},
        {QStringLiteral("components"),
         QJsonArray{QJsonObject{
             {QStringLiteral("issue_id"), QStringLiteral("example.issue.preservation")},
             {QStringLiteral("target_id"), QStringLiteral("example.target.preservation")},
             {QStringLiteral("scope"), QStringLiteral("whole")},
             {QStringLiteral("action"), QStringLiteral("dismiss")},
             {QStringLiteral("remand"), true},
             {QStringLiteral("authority_ids"),
              QJsonArray{QStringLiteral("example.authority.rule-one")}},
             {QStringLiteral("record_anchor_ids"),
              QJsonArray{QStringLiteral("example.record.entry-one"),
                         QStringLiteral("example.record.anchor.ja2")}},
         }}},
    };
    QCOMPARE(dispositionDigest(fixed_vector_case, fixed_vector_plan),
             QStringLiteral("d9c97181a59eb4a0fd79aa3fcad32bd9cd5e4128aad8a49a68384900a1eb5121"));
    for (int variant = 0; variant < 8; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), case_path, [variant](QJsonObject& document) {
            if (variant == 1) {
                document.remove(QStringLiteral("authored_disposition_plan_id"));
                return;
            }
            auto plans = document.value(QStringLiteral("disposition_plans")).toArray();
            auto plan = plans.at(0).toObject();
            auto components = plan.value(QStringLiteral("components")).toArray();
            auto component = components.at(0).toObject();
            if (variant == 0) {
                plan.insert(QStringLiteral("digest"), QString(64, QLatin1Char('0')));
            } else if (variant == 2) {
                components.push_back(component);
                plan.insert(QStringLiteral("components"), components);
            } else if (variant == 3) {
                component.insert(QStringLiteral("action"), QStringLiteral("affirm"));
                components.replace(0, component);
                plan.insert(QStringLiteral("components"), components);
            } else if (variant == 4) {
                component.insert(QStringLiteral("authority_ids"),
                                 QJsonArray{QStringLiteral("example.authority.judgment")});
                components.replace(0, component);
                plan.insert(QStringLiteral("components"), components);
            } else if (variant == 5) {
                component.insert(QStringLiteral("record_anchor_ids"),
                                 QJsonArray{QStringLiteral("example.record.anchor.missing")});
                components.replace(0, component);
                plan.insert(QStringLiteral("components"), components);
            } else if (variant == 6) {
                component.insert(QStringLiteral("target_id"),
                                 QStringLiteral("example.target.missing"));
                components.replace(0, component);
                plan.insert(QStringLiteral("components"), components);
            } else {
                auto issues = document.value(QStringLiteral("issues")).toArray();
                auto second_issue = issues.at(1).toObject();
                second_issue.insert(QStringLiteral("target_ids"),
                                    QJsonArray{QStringLiteral("example.target.preservation")});
                issues.replace(1, second_issue);
                document.insert(QStringLiteral("issues"), issues);
                auto second_component = components.at(1).toObject();
                second_component.insert(QStringLiteral("target_id"),
                                        QStringLiteral("example.target.preservation"));
                components.replace(1, second_component);
                plan.insert(QStringLiteral("components"), components);
            }
            plans.replace(0, plan);
            document.insert(QStringLiteral("disposition_plans"), plans);
            if (variant != 0) {
                refreshDispositionDigests(document);
            }
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir extra_field;
    QVERIFY(extra_field.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), extra_field.path()));
    QVERIFY(mutateResource(extra_field.path(), case_path, [](QJsonObject& document) {
        auto plans = document.value(QStringLiteral("disposition_plans")).toArray();
        auto plan = plans.at(0).toObject();
        auto components = plan.value(QStringLiteral("components")).toArray();
        auto component = components.at(0).toObject();
        component.insert(QStringLiteral("binding_effect"), QStringLiteral("binding"));
        components.replace(0, component);
        plan.insert(QStringLiteral("components"), components);
        plans.replace(0, plan);
        document.insert(QStringLiteral("disposition_plans"), plans);
    }));
    const auto malformed = PackReader::readDirectory(extra_field.path());
    QVERIFY(!malformed.has_value());
    QCOMPARE(malformed.error().code, ErrorCode::SchemaViolation);

    QTemporaryDir normalized;
    QVERIFY(normalized.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), normalized.path()));
    QVERIFY(mutateResource(normalized.path(), case_path, [](QJsonObject& document) {
        auto plans = document.value(QStringLiteral("disposition_plans")).toArray();
        auto plan = plans.at(0).toObject();
        auto components = plan.value(QStringLiteral("components")).toArray();
        auto first = components.at(0).toObject();
        const auto second = components.at(1).toObject();
        first.insert(QStringLiteral("record_anchor_ids"),
                     QJsonArray{QStringLiteral("example.record.anchor.ja2"),
                                QStringLiteral("example.record.entry-one")});
        plan.insert(QStringLiteral("finality"), QStringLiteral("nonfinal"));
        plan.insert(QStringLiteral("components"), QJsonArray{first, second});
        plans.replace(0, plan);
        document.insert(QStringLiteral("disposition_plans"), plans);
        refreshDispositionDigests(document);

        plans = document.value(QStringLiteral("disposition_plans")).toArray();
        plan = plans.at(0).toObject();
        plan.insert(QStringLiteral("components"), QJsonArray{second, first});
        plans.replace(0, plan);
        document.insert(QStringLiteral("disposition_plans"), plans);
    }));
    const auto normalized_loaded = PackReader::readDirectory(normalized.path());
    QVERIFY2(normalized_loaded.has_value(),
             normalized_loaded ? "" : qPrintable(normalized_loaded.error().message));
    const auto normalized_runtime = appellate::packs::loadRuntimePack(*normalized_loaded);
    QVERIFY2(normalized_runtime.has_value(),
             normalized_runtime ? "" : normalized_runtime.error().message.c_str());
    QCOMPARE(normalized_runtime->cases.front().definition.disposition_targets.size(),
             std::size_t{2});
    QCOMPARE(
        normalized_runtime->cases.front().definition.disposition_plans.front().components.size(),
        std::size_t{2});
    QCOMPARE(normalized_runtime->cases.front().definition.disposition_plans.front().finality,
             appellate::model::DispositionFinality::Nonfinal);

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto case_resource = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Case;
    });
    QVERIFY(case_resource != forged.resources.end());
    auto forged_plans =
        case_resource->document.value(QStringLiteral("disposition_plans")).toArray();
    auto forged_plan = forged_plans.at(0).toObject();
    forged_plan.insert(QStringLiteral("digest"), QString(64, QLatin1Char('0')));
    forged_plans.replace(0, forged_plan);
    case_resource->document.insert(QStringLiteral("disposition_plans"), forged_plans);
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);
}

void SchemaDispatchTest::enforcesStructuredFeatureBounds() {
    const auto case_path = QStringLiteral("resources/case.json");
    const auto workflow_path = QStringLiteral("resources/workflow.json");
    const auto set_component_count = [](QJsonObject& document, int count) {
        auto issues = document.value(QStringLiteral("issues")).toArray();
        auto issue = issues.at(0).toObject();
        QJsonArray targets;
        QJsonArray components;
        const auto template_plan =
            document.value(QStringLiteral("disposition_plans")).toArray().at(0).toObject();
        const auto template_component =
            template_plan.value(QStringLiteral("components")).toArray().at(0).toObject();
        for (int index = 0; index < count; ++index) {
            const auto target_id = QStringLiteral("example.target.boundary-%1").arg(index);
            targets.push_back(target_id);
            auto component = template_component;
            component.insert(QStringLiteral("target_id"), target_id);
            components.push_back(component);
        }
        issue.insert(QStringLiteral("target_ids"), targets);
        issues.replace(0, issue);
        document.insert(QStringLiteral("issues"), issues);
        auto plans = document.value(QStringLiteral("disposition_plans")).toArray();
        auto plan = plans.at(0).toObject();
        plan.insert(QStringLiteral("components"), components);
        plans.replace(0, plan);
        document.insert(QStringLiteral("disposition_plans"), plans);
        refreshDispositionDigests(document);
    };

    QTemporaryDir component_maximum;
    QVERIFY(component_maximum.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), component_maximum.path()));
    QVERIFY(mutateResource(component_maximum.path(), case_path,
                           [&](QJsonObject& document) { set_component_count(document, 32); }));
    const auto maximum_components = PackReader::readDirectory(component_maximum.path());
    QVERIFY2(maximum_components.has_value(),
             maximum_components ? "" : qPrintable(maximum_components.error().message));
    const auto maximum_components_runtime = appellate::packs::loadRuntimePack(*maximum_components);
    QVERIFY2(maximum_components_runtime.has_value(),
             maximum_components_runtime ? "" : maximum_components_runtime.error().message.c_str());
    QCOMPARE(maximum_components_runtime->cases.front()
                 .definition.disposition_plans.front()
                 .components.size(),
             std::size_t{32});

    QTemporaryDir component_overflow;
    QVERIFY(component_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), component_overflow.path()));
    QVERIFY(mutateResource(component_overflow.path(), case_path,
                           [&](QJsonObject& document) { set_component_count(document, 33); }));
    const auto too_many_components = PackReader::readDirectory(component_overflow.path());
    QVERIFY(!too_many_components.has_value());
    QCOMPARE(too_many_components.error().code, ErrorCode::SchemaViolation);

    const auto set_targets_per_issue = [](QJsonObject& document, int count) {
        auto issues = document.value(QStringLiteral("issues")).toArray();
        for (qsizetype issue_index = 0; issue_index < issues.size(); ++issue_index) {
            auto issue = issues.at(issue_index).toObject();
            QJsonArray targets;
            const auto prefix =
                issue_index == 0 ? QStringLiteral("preservation") : QStringLiteral("prejudice");
            for (int target_index = 0; target_index < count; ++target_index) {
                targets.push_back(
                    QStringLiteral("example.target.%1-%2").arg(prefix).arg(target_index));
            }
            issue.insert(QStringLiteral("target_ids"), targets);
            issues.replace(issue_index, issue);
        }
        document.insert(QStringLiteral("issues"), issues);
        auto plans = document.value(QStringLiteral("disposition_plans")).toArray();
        auto plan = plans.at(0).toObject();
        auto components = plan.value(QStringLiteral("components")).toArray();
        auto first = components.at(0).toObject();
        first.insert(QStringLiteral("target_id"), QStringLiteral("example.target.preservation-0"));
        components.replace(0, first);
        auto second = components.at(1).toObject();
        second.insert(QStringLiteral("target_id"), QStringLiteral("example.target.prejudice-0"));
        components.replace(1, second);
        plan.insert(QStringLiteral("components"), components);
        plans.replace(0, plan);
        document.insert(QStringLiteral("disposition_plans"), plans);
        refreshDispositionDigests(document);
    };

    QTemporaryDir target_maximum;
    QVERIFY(target_maximum.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), target_maximum.path()));
    QVERIFY(mutateResource(target_maximum.path(), case_path,
                           [&](QJsonObject& document) { set_targets_per_issue(document, 2048); }));
    const auto maximum_targets = PackReader::readDirectory(target_maximum.path());
    QVERIFY2(maximum_targets.has_value(),
             maximum_targets ? "" : qPrintable(maximum_targets.error().message));
    const auto maximum_targets_runtime = appellate::packs::loadRuntimePack(*maximum_targets);
    QVERIFY2(maximum_targets_runtime.has_value(),
             maximum_targets_runtime ? "" : maximum_targets_runtime.error().message.c_str());
    QCOMPARE(maximum_targets_runtime->cases.front().definition.disposition_targets.size(),
             std::size_t{4096});

    QTemporaryDir target_overflow;
    QVERIFY(target_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), target_overflow.path()));
    QVERIFY(mutateResource(target_overflow.path(), case_path,
                           [&](QJsonObject& document) { set_targets_per_issue(document, 2049); }));
    const auto too_many_targets = PackReader::readDirectory(target_overflow.path());
    QVERIFY(!too_many_targets.has_value());
    QCOMPARE(too_many_targets.error().code, ErrorCode::CrossReferenceFailure);

    const auto set_precondition_count = [](QJsonObject& document, int count) {
        QJsonArray preconditions;
        for (int index = 0; index < count; ++index) {
            preconditions.push_back(QJsonObject{
                {QStringLiteral("kind"), QStringLiteral("order_disposition")},
                {QStringLiteral("order_id"),
                 QStringLiteral("example.order.boundary-%1").arg(index)},
                {QStringLiteral("disposition"), QStringLiteral("granted")},
            });
        }
        auto operations = document.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() ==
                QStringLiteral("example.operation.issue-judgment")) {
                operation.insert(QStringLiteral("preconditions"), preconditions);
                operations.replace(index, operation);
                break;
            }
        }
        document.insert(QStringLiteral("operations"), operations);
    };

    QTemporaryDir precondition_maximum;
    QVERIFY(precondition_maximum.isValid());
    QVERIFY(
        copyTree(fixture(QStringLiteral("full-resource-pack-v2")), precondition_maximum.path()));
    QVERIFY(mutateResource(precondition_maximum.path(), workflow_path,
                           [&](QJsonObject& document) { set_precondition_count(document, 32); }));
    const auto maximum_preconditions = PackReader::readDirectory(precondition_maximum.path());
    QVERIFY2(maximum_preconditions.has_value(),
             maximum_preconditions ? "" : qPrintable(maximum_preconditions.error().message));
    const auto maximum_preconditions_runtime =
        appellate::packs::loadRuntimePack(*maximum_preconditions);
    QVERIFY2(maximum_preconditions_runtime.has_value(),
             maximum_preconditions_runtime ? ""
                                           : maximum_preconditions_runtime.error().message.c_str());
    const auto maximum_judgment =
        std::ranges::find(maximum_preconditions_runtime->cases.front().workflow.operations,
                          appellate::model::WorkflowOperationId{"example.operation.issue-judgment"},
                          &appellate::model::WorkflowOperation::id);
    QVERIFY(maximum_judgment !=
            maximum_preconditions_runtime->cases.front().workflow.operations.end());
    QCOMPARE(maximum_judgment->preconditions.size(), std::size_t{32});

    QTemporaryDir precondition_overflow;
    QVERIFY(precondition_overflow.isValid());
    QVERIFY(
        copyTree(fixture(QStringLiteral("full-resource-pack-v2")), precondition_overflow.path()));
    QVERIFY(mutateResource(precondition_overflow.path(), workflow_path,
                           [&](QJsonObject& document) { set_precondition_count(document, 33); }));
    const auto too_many_preconditions = PackReader::readDirectory(precondition_overflow.path());
    QVERIFY(!too_many_preconditions.has_value());
    QCOMPARE(too_many_preconditions.error().code, ErrorCode::SchemaViolation);

    const auto set_plan_count = [](QJsonObject& document, int count) {
        const auto template_plan =
            document.value(QStringLiteral("disposition_plans")).toArray().at(0).toObject();
        QJsonArray plans;
        for (int index = 0; index < count; ++index) {
            auto plan = template_plan;
            plan.insert(QStringLiteral("plan_id"),
                        QStringLiteral("example.disposition.boundary-%1").arg(index));
            plans.push_back(plan);
        }
        document.insert(QStringLiteral("disposition_plans"), plans);
        document.insert(QStringLiteral("authored_disposition_plan_id"),
                        QStringLiteral("example.disposition.boundary-0"));
        refreshDispositionDigests(document);
    };

    QTemporaryDir plan_maximum;
    QVERIFY(plan_maximum.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), plan_maximum.path()));
    QVERIFY(mutateResource(plan_maximum.path(), case_path,
                           [&](QJsonObject& document) { set_plan_count(document, 64); }));
    const auto maximum_plans = PackReader::readDirectory(plan_maximum.path());
    QVERIFY2(maximum_plans.has_value(),
             maximum_plans ? "" : qPrintable(maximum_plans.error().message));
    const auto maximum_plans_runtime = appellate::packs::loadRuntimePack(*maximum_plans);
    QVERIFY2(maximum_plans_runtime.has_value(),
             maximum_plans_runtime ? "" : maximum_plans_runtime.error().message.c_str());
    QCOMPARE(maximum_plans_runtime->cases.front().definition.disposition_plans.size(),
             std::size_t{64});

    QTemporaryDir plan_overflow;
    QVERIFY(plan_overflow.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), plan_overflow.path()));
    QVERIFY(mutateResource(plan_overflow.path(), case_path,
                           [&](QJsonObject& document) { set_plan_count(document, 65); }));
    const auto too_many_plans = PackReader::readDirectory(plan_overflow.path());
    QVERIFY(!too_many_plans.has_value());
    QCOMPARE(too_many_plans.error().code, ErrorCode::SchemaViolation);

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto case_resource = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Case;
    });
    QVERIFY(case_resource != forged.resources.end());
    set_targets_per_issue(case_resource->document, 2049);
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);
}

void SchemaDispatchTest::closureAndRuntimeRejectForgedCapabilityCoverage() {
    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));

    auto forged = *loaded;
    forged.required_capabilities.erase(
        std::remove_if(forged.required_capabilities.begin(), forged.required_capabilities.end(),
                       [](const auto& capability) {
                           return capability.id != "workbench.pack.declarative-resources";
                       }),
        forged.required_capabilities.end());
    const auto closure = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!closure.has_value());
    QCOMPARE(closure.error().code, ErrorCode::UnsupportedCapability);

    const auto runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!runtime.has_value());
    QCOMPARE(runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);
}

void SchemaDispatchTest::rejectsInvalidCanonicalAuthorityMetadata() {
    const auto authority_path = QStringLiteral("resources/authority-set.json");
    for (int variant = 0; variant < 4; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), authority_path, [variant](QJsonObject& document) {
            auto authorities = document.value(QStringLiteral("authorities")).toArray();
            auto authority = authorities.at(0).toObject();
            if (variant == 0) {
                authority.remove(QStringLiteral("issuing_body_id"));
            } else if (variant == 1) {
                authority.insert(QStringLiteral("source_url"),
                                 QStringLiteral("http://example.invalid/rules/1"));
            } else if (variant == 2) {
                authority.insert(QStringLiteral("checked_on"), QStringLiteral("2025-12-31"));
            } else {
                authority.insert(QStringLiteral("source_version"), QStringLiteral("2026-01-02"));
            }
            authorities.replace(0, authority);
            document.insert(QStringLiteral("authorities"), authorities);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code,
                 variant < 2 ? ErrorCode::SchemaViolation : ErrorCode::CrossReferenceFailure);
    }

    const std::array noncanonical_urls{
        QStringLiteral("https://Example.invalid/rules/1"),
        QStringLiteral("https://example.invalid:443/rules/1"),
        QStringLiteral("https://example.invalid/rules/1#section"),
        QStringLiteral("https://example.invalid/rules/%2f"),
    };
    for (const auto& source_url : noncanonical_urls) {
        QVERIFY(
            !appellate::model::isCanonicalAuthoritySourceUrl(source_url.toUtf8().toStdString()));
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), authority_path, [&](QJsonObject& document) {
            auto authorities = document.value(QStringLiteral("authorities")).toArray();
            auto authority = authorities.at(0).toObject();
            authority.insert(QStringLiteral("source_url"), source_url);
            authorities.replace(0, authority);
            document.insert(QStringLiteral("authorities"), authorities);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QVERIFY(result.error().code == ErrorCode::SchemaViolation ||
                result.error().code == ErrorCode::CrossReferenceFailure);
    }

    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto authority_set = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::AuthoritySet;
    });
    QVERIFY(authority_set != forged.resources.end());
    auto authorities = authority_set->document.value(QStringLiteral("authorities")).toArray();
    auto authority = authorities.at(0).toObject();
    authority.insert(QStringLiteral("source_url"), noncanonical_urls.front());
    authorities.replace(0, authority);
    authority_set->document.insert(QStringLiteral("authorities"), authorities);
    const auto runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!runtime.has_value());
    QCOMPARE(runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidResource);

    const QString unicode_text(2000, QChar{0xD55C});
    const QString unicode_locator(1024, QChar{0xD55C});
    QVERIFY(appellate::model::isCanonicalAuthorityText(unicode_text.toUtf8().toStdString(), 4096));
    QVERIFY(
        appellate::model::isCanonicalAuthorityText(unicode_locator.toUtf8().toStdString(), 1024));
    QTemporaryDir unicode_pack;
    QVERIFY(unicode_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unicode_pack.path()));
    QVERIFY(stripGroundedQuestions(unicode_pack.path()));
    QVERIFY(mutateResource(unicode_pack.path(), authority_path, [&](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("citation"), unicode_text);
        first.insert(QStringLiteral("proposition"), unicode_text);
        first.insert(QStringLiteral("locator"), unicode_locator);
        values.replace(0, first);
        document.insert(QStringLiteral("authorities"), values);
    }));
    const auto unicode_loaded = PackReader::readDirectory(unicode_pack.path());
    QVERIFY2(unicode_loaded.has_value(),
             unicode_loaded ? "" : qPrintable(unicode_loaded.error().message));
    const auto unicode_runtime = appellate::packs::loadRuntimePack(*unicode_loaded);
    QVERIFY2(unicode_runtime.has_value(),
             unicode_runtime ? "" : unicode_runtime.error().message.c_str());
    const auto unicode_authority = std::ranges::find_if(
        unicode_runtime->cases.front().workflow.operations, [](const auto& operation) {
            return operation.authority.primary.id.value == "example.authority.rule-one";
        });
    QVERIFY(unicode_authority != unicode_runtime->cases.front().workflow.operations.end());
    QCOMPARE(unicode_authority->authority.primary.citation, unicode_text.toUtf8().toStdString());
    QCOMPARE(unicode_authority->authority.primary.provenance->locator,
             unicode_locator.toUtf8().toStdString());

    QTemporaryDir overlong_locator_pack;
    QVERIFY(overlong_locator_pack.isValid());
    QVERIFY(
        copyTree(fixture(QStringLiteral("full-resource-pack-v2")), overlong_locator_pack.path()));
    QVERIFY(stripGroundedQuestions(overlong_locator_pack.path()));
    QVERIFY(mutateResource(overlong_locator_pack.path(), authority_path, [](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("locator"), QString(1025, QChar{0xD55C}));
        values.replace(0, first);
        document.insert(QStringLiteral("authorities"), values);
    }));
    const auto overlong_locator = PackReader::readDirectory(overlong_locator_pack.path());
    QVERIFY(!overlong_locator.has_value());
    QCOMPARE(overlong_locator.error().code, ErrorCode::SchemaViolation);

    QTemporaryDir overlong_pack;
    QVERIFY(overlong_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), overlong_pack.path()));
    QVERIFY(mutateResource(overlong_pack.path(), authority_path, [&](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("citation"), QString(4097, QChar{0xD55C}));
        values.replace(0, first);
        document.insert(QStringLiteral("authorities"), values);
    }));
    const auto overlong = PackReader::readDirectory(overlong_pack.path());
    QVERIFY(!overlong.has_value());
    QCOMPARE(overlong.error().code, ErrorCode::SchemaViolation);

    const auto jurisdiction_id = QStringLiteral("test.") + QString(155, QLatin1Char('a'));
    QCOMPARE(jurisdiction_id.size(), 160);
    QTemporaryDir maximum_id_pack;
    QVERIFY(maximum_id_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), maximum_id_pack.path()));
    QVERIFY(stripGroundedQuestions(maximum_id_pack.path()));
    QVERIFY(mutateResource(maximum_id_pack.path(), authority_path, [&](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("jurisdiction_id"), jurisdiction_id);
        values.replace(0, first);
        document.insert(QStringLiteral("authorities"), values);
    }));
    const auto maximum_id = PackReader::readDirectory(maximum_id_pack.path());
    QVERIFY2(maximum_id.has_value(), maximum_id ? "" : qPrintable(maximum_id.error().message));
    const auto maximum_id_runtime = appellate::packs::loadRuntimePack(*maximum_id);
    QVERIFY2(maximum_id_runtime.has_value(),
             maximum_id_runtime ? "" : maximum_id_runtime.error().message.c_str());

    auto forged_pack_id = *maximum_id;
    forged_pack_id.revision.id.value = "test." + std::string(124, 'a');
    QCOMPARE(forged_pack_id.revision.id.value.size(), std::size_t{129});
    const auto forged_pack_id_runtime = appellate::packs::loadRuntimePack(forged_pack_id);
    QVERIFY(!forged_pack_id_runtime.has_value());
    QCOMPARE(forged_pack_id_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::InvalidPack);

    QTemporaryDir overlong_id_pack;
    QVERIFY(overlong_id_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), overlong_id_pack.path()));
    QVERIFY(mutateResource(overlong_id_pack.path(), authority_path, [&](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("jurisdiction_id"), jurisdiction_id + QLatin1Char('a'));
        values.replace(0, first);
        document.insert(QStringLiteral("authorities"), values);
    }));
    const auto overlong_id = PackReader::readDirectory(overlong_id_pack.path());
    QVERIFY(!overlong_id.has_value());
    QCOMPARE(overlong_id.error().code, ErrorCode::SchemaViolation);
}

void SchemaDispatchTest::rejectsUnresolvedAndDuplicateAuthoritySelections() {
    const auto workflow_path = QStringLiteral("resources/workflow.json");
    for (int variant = 0; variant < 3; ++variant) {
        QTemporaryDir pack;
        QVERIFY(pack.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
        QVERIFY(mutateResource(pack.path(), workflow_path, [variant](QJsonObject& document) {
            auto operations = document.value(QStringLiteral("operations")).toArray();
            auto operation = operations.at(0).toObject();
            auto authority = operation.value(QStringLiteral("authority")).toObject();
            if (variant == 0) {
                authority.insert(QStringLiteral("primary_authority_id"),
                                 QStringLiteral("example.authority.missing"));
            } else if (variant == 1) {
                QJsonArray supporting;
                supporting.push_back(authority.value(QStringLiteral("primary_authority_id")));
                authority.insert(QStringLiteral("supporting_authority_ids"), supporting);
            } else {
                authority.insert(QStringLiteral("supporting_authority_ids"),
                                 QJsonArray{QStringLiteral("example.authority.deficiency"),
                                            QStringLiteral("example.authority.deficiency")});
            }
            operation.insert(QStringLiteral("authority"), authority);
            operations.replace(0, operation);
            document.insert(QStringLiteral("operations"), operations);
        }));
        const auto result = PackReader::readDirectory(pack.path());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code,
                 variant == 2 ? ErrorCode::SchemaViolation : ErrorCode::CrossReferenceFailure);
    }

    QTemporaryDir duplicate;
    QVERIFY(duplicate.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), duplicate.path()));
    QVERIFY(mutateResource(duplicate.path(), QStringLiteral("resources/authority-set.json"),
                           [](QJsonObject& document) {
                               auto authorities =
                                   document.value(QStringLiteral("authorities")).toArray();
                               auto conflicting = authorities.at(0).toObject();
                               conflicting.insert(QStringLiteral("citation"),
                                                  QStringLiteral("Conflicting Rule 1"));
                               authorities.push_back(conflicting);
                               document.insert(QStringLiteral("authorities"), authorities);
                           }));
    const auto duplicate_result = PackReader::readDirectory(duplicate.path());
    QVERIFY(!duplicate_result.has_value());
    QCOMPARE(duplicate_result.error().code, ErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::resolvesCanonicalAuthorityAcrossDependencyGraph() {
    const auto loaded = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));

    auto root = *loaded;
    auto dependency = *loaded;
    root.revision.id.value = "example.full.authority-consumer";
    root.revision.digest = std::string(64, 'a');
    dependency.revision.id.value = "example.full.authority-provider";
    dependency.revision.digest = std::string(64, 'b');
    std::erase_if(root.resources, [](const appellate::packs::ValidatedResource& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::AuthoritySet;
    });
    std::erase_if(dependency.resources, [](const appellate::packs::ValidatedResource& resource) {
        return resource.descriptor.kind != appellate::model::ResourceKind::AuthoritySet;
    });
    dependency.judge_profiles.clear();
    dependency.blobs.clear();
    root.graph_state = appellate::packs::PackGraphState::DeferredReferences;
    dependency.graph_state = appellate::packs::PackGraphState::StandaloneValidated;
    const std::array<const appellate::packs::LoadedPack*, 1> closure{&dependency};
    const auto resolved = PackReader::validateResolvedGraph(root, closure);
    QVERIFY2(resolved.has_value(), resolved ? "" : qPrintable(resolved.error().message));

    auto duplicate_provider = dependency;
    duplicate_provider.revision.id.value = "example.full.authority-conflict";
    duplicate_provider.revision.digest = std::string(64, 'c');
    duplicate_provider.resources.front().descriptor.id = "example.authorities.conflicting";
    auto conflicting_authority = duplicate_provider.resources.front().document;
    conflicting_authority.insert(QStringLiteral("resource_id"),
                                 QStringLiteral("example.authorities.conflicting"));
    auto authorities = conflicting_authority.value(QStringLiteral("authorities")).toArray();
    auto first = authorities.at(0).toObject();
    first.insert(QStringLiteral("citation"), QStringLiteral("Conflicting canonical citation"));
    authorities.replace(0, first);
    conflicting_authority.insert(QStringLiteral("authorities"), authorities);
    duplicate_provider.resources.front().document = conflicting_authority;
    const std::array<const appellate::packs::LoadedPack*, 2> duplicate_closure{&dependency,
                                                                               &duplicate_provider};
    const auto duplicate = PackReader::validateResolvedGraph(root, duplicate_closure);
    QVERIFY(!duplicate.has_value());
    QCOMPARE(duplicate.error().code, ErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::validatesExactRealismEvidenceAndReviewExclusion() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(prepareRealismEvidencePack(pack.path()));

    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    const auto review = std::ranges::find_if(loaded->resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    QVERIFY(review != loaded->resources.end());
    const auto evidence = review->document.value(QStringLiteral("evidence")).toObject();
    const auto closure_digest = evidence.value(QStringLiteral("closure_digest")).toString();
    QCOMPARE(closure_digest.size(), 64);
    QCOMPARE(evidence.value(QStringLiteral("packs")).toArray().size(), 1);
    QCOMPARE(evidence.value(QStringLiteral("resources")).toArray().size(), 12);
    QCOMPARE(evidence.value(QStringLiteral("blobs")).toArray().size(), 1);
    QCOMPARE(evidence.value(QStringLiteral("traces"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("event_count"))
                 .toInt(),
             2);

    const auto first_revision = loaded->revision;
    QVERIFY(mutateResource(
        pack.path(), QStringLiteral("resources/realism-review.json"), [](QJsonObject& document) {
            auto uncertainties = document.value(QStringLiteral("known_uncertainty")).toArray();
            auto uncertainty = uncertainties.first().toObject();
            uncertainty.insert(
                QStringLiteral("summary"),
                QStringLiteral("The fictional authorities require tracked remediation."));
            uncertainty.insert(QStringLiteral("blocking"), true);
            uncertainty.insert(
                QStringLiteral("remediation_issue"),
                QStringLiteral("https://github.com/junepark678/appellate-workbench/issues/27"));
            uncertainties.replace(0, uncertainty);
            document.insert(QStringLiteral("known_uncertainty"), uncertainties);
        }));
    const auto review_only_change = PackReader::readDirectory(pack.path());
    QVERIFY2(review_only_change.has_value(),
             review_only_change ? "" : qPrintable(review_only_change.error().message));
    QVERIFY(review_only_change->revision != first_revision);
    const auto changed_review =
        std::ranges::find_if(review_only_change->resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
        });
    QVERIFY(changed_review != review_only_change->resources.end());
    QCOMPARE(changed_review->document.value(QStringLiteral("evidence"))
                 .toObject()
                 .value(QStringLiteral("closure_digest"))
                 .toString(),
             closure_digest);
}

void SchemaDispatchTest::rejectsTamperedAndIncompleteRealismEvidence() {
    QTemporaryDir valid;
    QVERIFY(valid.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), valid.path()));
    QVERIFY(prepareRealismEvidencePack(valid.path()));

    struct Mutation final {
        const char* name;
        std::function<void(QJsonObject&)> apply;
        ErrorCode expected;
    };
    const std::vector<Mutation> mutations{
        {"closure digest",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             evidence.insert(QStringLiteral("closure_digest"), QString(64, u'0'));
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"resource descriptor",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto resources = evidence.value(QStringLiteral("resources")).toArray();
             auto resource = resources.first().toObject();
             resource.insert(QStringLiteral("sha256"), QString(64, u'1'));
             resources.replace(0, resource);
             evidence.insert(QStringLiteral("resources"), resources);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"missing resource binding",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto resources = evidence.value(QStringLiteral("resources")).toArray();
             resources.removeFirst();
             evidence.insert(QStringLiteral("resources"), resources);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"wrong blob owner",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto blobs = evidence.value(QStringLiteral("blobs")).toArray();
             auto blob = blobs.first().toObject();
             blob.insert(QStringLiteral("owner_pack_id"), QStringLiteral("example.wrong.owner"));
             blobs.replace(0, blob);
             evidence.insert(QStringLiteral("blobs"), blobs);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"pack topology",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto packs = evidence.value(QStringLiteral("packs")).toArray();
             auto pack = packs.first().toObject();
             pack.insert(
                 QStringLiteral("dependencies"),
                 QJsonArray{QJsonObject{
                     {QStringLiteral("pack_id"), QStringLiteral("example.forged.dependency")},
                     {QStringLiteral("version"), QStringLiteral("1.0.0")},
                 }});
             packs.replace(0, pack);
             evidence.insert(QStringLiteral("packs"), packs);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"journal digest",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto traces = evidence.value(QStringLiteral("traces")).toArray();
             auto trace = traces.first().toObject();
             trace.insert(QStringLiteral("journal_sha256"), QString(64, u'2'));
             traces.replace(0, trace);
             evidence.insert(QStringLiteral("traces"), traces);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"self-consistent but unreplayable journal",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto traces = evidence.value(QStringLiteral("traces")).toArray();
             auto trace = traces.first().toObject();
             auto journal = trace.value(QStringLiteral("journal")).toArray();
             auto entry = journal.first().toObject();
             const auto encoded =
                 entry.value(QStringLiteral("command_base64")).toString().toLatin1();
             auto command = QJsonDocument::fromJson(QByteArray::fromBase64(encoded)).object();
             auto payload = command.value(QStringLiteral("payload")).toObject();
             payload.insert(QStringLiteral("command_id"),
                            QStringLiteral("example.command.forged-replay"));
             command.insert(QStringLiteral("payload"), payload);
             entry.insert(QStringLiteral("command_base64"),
                          QString::fromLatin1(
                              QJsonDocument(command).toJson(QJsonDocument::Compact).toBase64()));
             journal.replace(0, entry);
             trace.insert(QStringLiteral("journal"), journal);
             const auto journal_digest = realismJournalDigest(journal);
             if (journal_digest) {
                 trace.insert(QStringLiteral("journal_sha256"), *journal_digest);
             }
             trace.insert(
                 QStringLiteral("digest"),
                 realismTraceDigest(document.value(QStringLiteral("case_id")).toString(), trace));
             traces.replace(0, trace);
             evidence.insert(QStringLiteral("traces"), traces);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"unknown executed operation",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto traces = evidence.value(QStringLiteral("traces")).toArray();
             auto trace = traces.first().toObject();
             auto operations = trace.value(QStringLiteral("operation_ids")).toArray();
             operations.replace(0, QStringLiteral("example.operation.not-in-workflow"));
             trace.insert(QStringLiteral("operation_ids"), operations);
             trace.insert(
                 QStringLiteral("digest"),
                 realismTraceDigest(document.value(QStringLiteral("case_id")).toString(), trace));
             traces.replace(0, trace);
             evidence.insert(QStringLiteral("traces"), traces);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"event operation count",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto traces = evidence.value(QStringLiteral("traces")).toArray();
             auto trace = traces.first().toObject();
             trace.insert(QStringLiteral("event_count"), 3);
             trace.insert(
                 QStringLiteral("digest"),
                 realismTraceDigest(document.value(QStringLiteral("case_id")).toString(), trace));
             traces.replace(0, trace);
             evidence.insert(QStringLiteral("traces"), traces);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"record check digest",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto checks = evidence.value(QStringLiteral("record_checks")).toArray();
             auto check = checks.first().toObject();
             check.insert(QStringLiteral("digest"), QString(64, u'3'));
             checks.replace(0, check);
             evidence.insert(QStringLiteral("record_checks"), checks);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"unknown authority",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto authorities = evidence.value(QStringLiteral("authorities")).toArray();
             auto authority = authorities.first().toObject();
             authority.insert(QStringLiteral("authority_id"),
                              QStringLiteral("example.authority.not-in-closure"));
             authorities.replace(0, authority);
             evidence.insert(QStringLiteral("authorities"), authorities);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"missing dimension evidence",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto dimensions = evidence.value(QStringLiteral("dimension_evidence")).toObject();
             dimensions.insert(QStringLiteral("procedural_law"), QJsonArray{});
             evidence.insert(QStringLiteral("dimension_evidence"), dimensions);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::CrossReferenceFailure},
        {"self review level three",
         [](QJsonObject& document) {
             auto dimensions = document.value(QStringLiteral("dimensions")).toObject();
             dimensions.insert(QStringLiteral("procedural_law"), 3);
             document.insert(QStringLiteral("dimensions"), dimensions);
         },
         ErrorCode::CrossReferenceFailure},
        {"blocking uncertainty without issue",
         [](QJsonObject& document) {
             auto uncertainties = document.value(QStringLiteral("known_uncertainty")).toArray();
             auto uncertainty = uncertainties.first().toObject();
             uncertainty.insert(QStringLiteral("blocking"), true);
             uncertainty.remove(QStringLiteral("remediation_issue"));
             uncertainties.replace(0, uncertainty);
             document.insert(QStringLiteral("known_uncertainty"), uncertainties);
         },
         ErrorCode::CrossReferenceFailure},
        {"typed shape without evidence",
         [](QJsonObject& document) { document.remove(QStringLiteral("evidence")); },
         ErrorCode::CrossReferenceFailure},
        {"all typed evidence stripped",
         [](QJsonObject& document) {
             document.remove(QStringLiteral("evidence"));
             document.remove(QStringLiteral("reviewer"));
             document.insert(QStringLiteral("known_uncertainty"),
                             QJsonArray{QStringLiteral("stripped legacy-shaped claim")});
             auto dimensions = document.value(QStringLiteral("dimensions")).toObject();
             dimensions.insert(QStringLiteral("procedural_law"), 3);
             document.insert(QStringLiteral("dimensions"), dimensions);
         },
         ErrorCode::CrossReferenceFailure},
        {"empty trace evidence",
         [](QJsonObject& document) {
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             evidence.insert(QStringLiteral("traces"), QJsonArray{});
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::SchemaViolation},
        {"too many dimension evidence references",
         [](QJsonObject& document) {
             QJsonArray references;
             for (int index = 0; index < 513; ++index) {
                 references.push_back(QStringLiteral("example.evidence.excess.%1").arg(index));
             }
             auto evidence = document.value(QStringLiteral("evidence")).toObject();
             auto dimensions = evidence.value(QStringLiteral("dimension_evidence")).toObject();
             dimensions.insert(QStringLiteral("procedural_law"), references);
             evidence.insert(QStringLiteral("dimension_evidence"), dimensions);
             document.insert(QStringLiteral("evidence"), evidence);
         },
         ErrorCode::SchemaViolation},
    };

    for (const auto& mutation : mutations) {
        QTemporaryDir candidate;
        QVERIFY2(candidate.isValid(), mutation.name);
        QVERIFY2(copyTree(valid.path(), candidate.path()), mutation.name);
        QVERIFY2(mutateResource(candidate.path(), QStringLiteral("resources/realism-review.json"),
                                mutation.apply),
                 mutation.name);
        const auto result = PackReader::readDirectory(candidate.path());
        QVERIFY2(!result.has_value(), mutation.name);
        QCOMPARE(result.error().code, mutation.expected);
    }

    QTemporaryDir changed_subject;
    QVERIFY(changed_subject.isValid());
    QVERIFY(copyTree(valid.path(), changed_subject.path()));
    QVERIFY(mutateResource(changed_subject.path(), QStringLiteral("resources/court.json"),
                           [](QJsonObject& court) {
                               court.insert(QStringLiteral("name"),
                                            QStringLiteral("Changed Fictional Court of Appeals"));
                           }));
    const auto stale_after_subject_change = PackReader::readDirectory(changed_subject.path());
    QVERIFY(!stale_after_subject_change.has_value());
    QCOMPARE(stale_after_subject_change.error().code, ErrorCode::CrossReferenceFailure);

    QTemporaryDir underdeclared;
    QVERIFY(underdeclared.isValid());
    QVERIFY(copyTree(valid.path(), underdeclared.path()));
    QVERIFY(mutateManifest(underdeclared.path(), [](QJsonObject& manifest) {
        QJsonArray retained;
        for (const auto& value :
             manifest.value(QStringLiteral("required_capabilities")).toArray()) {
            if (value.toObject().value(QStringLiteral("id")).toString() !=
                QStringLiteral("workbench.pack.realism-evidence")) {
                retained.push_back(value);
            }
        }
        manifest.insert(QStringLiteral("required_capabilities"), retained);
    }));
    const auto missing_capability = PackReader::readDirectory(underdeclared.path());
    QVERIFY(!missing_capability.has_value());
    QCOMPARE(missing_capability.error().code, ErrorCode::UnsupportedCapability);

    const auto loaded = PackReader::readDirectory(valid.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));
    auto forged = *loaded;
    const auto forged_review = std::ranges::find_if(forged.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    QVERIFY(forged_review != forged.resources.end());
    forged_review->document.remove(QStringLiteral("evidence"));
    forged_review->document.remove(QStringLiteral("reviewer"));
    forged_review->document.insert(
        QStringLiteral("known_uncertainty"),
        QJsonArray{QStringLiteral("in-memory evidence stripping must fail closed")});
    const auto forged_graph = PackReader::validateResolvedGraph(forged, {});
    QVERIFY(!forged_graph.has_value());
    QCOMPARE(forged_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_runtime = appellate::packs::loadRuntimePack(forged);
    QVERIFY(!forged_runtime.has_value());
    QCOMPARE(forged_runtime.error().code, appellate::packs::RuntimePackErrorCode::InvalidPack);

    auto forged_pinned_revision = *loaded;
    const auto forged_pinned_review =
        std::ranges::find_if(forged_pinned_revision.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
        });
    QVERIFY(forged_pinned_review != forged_pinned_revision.resources.end());
    const auto exact_legacy_review = readObject(fixture(
        QStringLiteral("full-resource-pack-v2-pre-27-overlay/resources/realism-review.json")));
    QVERIFY(!exact_legacy_review.isEmpty());
    forged_pinned_review->document = exact_legacy_review;
    forged_pinned_revision.revision.digest =
        "a9c912ad7e23620f9a5c9f5fb81c9edabe1d00010551c4636e8a621b00655bd4";
    const auto forged_pinned_graph = PackReader::validateResolvedGraph(forged_pinned_revision, {});
    QVERIFY(!forged_pinned_graph.has_value());
    QCOMPARE(forged_pinned_graph.error().code, ErrorCode::CrossReferenceFailure);
    const auto forged_pinned_runtime = appellate::packs::loadRuntimePack(forged_pinned_revision);
    QVERIFY(!forged_pinned_runtime.has_value());
    QCOMPARE(forged_pinned_runtime.error().code,
             appellate::packs::RuntimePackErrorCode::InvalidPack);
}

void validatesManualDetachedReviewCompatibility() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(prepareRealismEvidencePack(pack.path()));
    const auto loaded = PackReader::readDirectory(pack.path());
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error().message));

    auto subject = *loaded;
    const auto source_review = std::ranges::find_if(subject.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    QVERIFY(source_review != subject.resources.end());
    const auto detached_resource = *source_review;
    std::erase_if(subject.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    std::erase_if(subject.required_capabilities, [](const auto& capability) {
        return capability.id == "workbench.pack.realism-evidence";
    });
    subject.revision.id.value = "example.subject.detached";
    subject.revision.version = "2.1.0";
    subject.revision.digest = std::string(64, 'a');

    appellate::packs::LoadedPack leaf{
        2,
        appellate::model::PackRevision{appellate::model::PackId{"example.subject.support"}, "1.0.0",
                                       std::string(64, 'b')},
        {appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2}},
        {},
        {},
        {},
        {},
        appellate::packs::PackGraphState::StandaloneValidated,
    };
    subject.dependencies = {appellate::model::PackDependency{leaf.revision}};
    subject.graph_state = appellate::packs::PackGraphState::DeferredReferences;

    const std::array<const appellate::packs::LoadedPack*, 2> subject_closure{&leaf, &subject};
    const auto independent_document =
        buildRealismReview(detached_resource, subject, subject_closure, true);
    QVERIFY(independent_document.has_value());
    auto independent_resource = detached_resource;
    independent_resource.document = *independent_document;

    appellate::packs::LoadedPack review_pack{
        2,
        appellate::model::PackRevision{appellate::model::PackId{"example.review.detached"}, "1.0.0",
                                       std::string(64, 'c')},
        {
            appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2},
            appellate::model::RequiredCapability{"workbench.pack.realism-evidence", 1},
        },
        {appellate::model::PackDependency{subject.revision}},
        {independent_resource},
        {},
        {},
        appellate::packs::PackGraphState::DeferredReferences,
    };
    const auto valid = PackReader::validateResolvedGraph(review_pack, subject_closure);
    QVERIFY2(valid.has_value(), valid ? "" : qPrintable(valid.error().message));

    auto wrong_pin = review_pack;
    wrong_pin.dependencies.front().revision.digest = std::string(64, 'd');
    const auto wrong_exact_revision = PackReader::validateResolvedGraph(wrong_pin, subject_closure);
    QVERIFY(!wrong_exact_revision.has_value());
    QCOMPARE(wrong_exact_revision.error().code, ErrorCode::CrossReferenceFailure);

    auto missing_pin = review_pack;
    missing_pin.dependencies.clear();
    const auto missing_direct_dependency =
        PackReader::validateResolvedGraph(missing_pin, subject_closure);
    QVERIFY(!missing_direct_dependency.has_value());
    QCOMPARE(missing_direct_dependency.error().code, ErrorCode::CrossReferenceFailure);

    auto missing_reviewer = review_pack;
    auto missing_reviewer_document = missing_reviewer.resources.front().document;
    missing_reviewer_document.remove(QStringLiteral("reviewer"));
    missing_reviewer.resources.front().document = missing_reviewer_document;
    const auto reviewer_metadata_required =
        PackReader::validateResolvedGraph(missing_reviewer, subject_closure);
    QVERIFY(!reviewer_metadata_required.has_value());
    QCOMPARE(reviewer_metadata_required.error().code, ErrorCode::CrossReferenceFailure);

    auto wrong_owner = review_pack;
    auto wrong_owner_document = wrong_owner.resources.front().document;
    wrong_owner_document.insert(QStringLiteral("review_state"), QStringLiteral("self_reviewed"));
    auto dimensions = wrong_owner_document.value(QStringLiteral("dimensions")).toObject();
    for (const auto* name :
         {"procedural_law", "deadlines_authority", "record_consistency", "consequences",
          "oral_argument", "bench_differentiation", "provenance"}) {
        dimensions.insert(QLatin1StringView(name), 2);
    }
    wrong_owner_document.insert(QStringLiteral("dimensions"), dimensions);
    wrong_owner.resources.front().document = wrong_owner_document;
    const auto colocated_self_required =
        PackReader::validateResolvedGraph(wrong_owner, subject_closure);
    QVERIFY(!colocated_self_required.has_value());
    QCOMPARE(colocated_self_required.error().code, ErrorCode::CrossReferenceFailure);

    auto non_detached = review_pack;
    const auto form = std::ranges::find_if(subject.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::Form;
    });
    QVERIFY(form != subject.resources.end());
    auto extra = *form;
    extra.descriptor.id = "example.form.detached-extra";
    extra.document.insert(QStringLiteral("resource_id"),
                          QStringLiteral("example.form.detached-extra"));
    non_detached.resources.push_back(std::move(extra));
    const auto review_resources_only =
        PackReader::validateResolvedGraph(non_detached, subject_closure);
    QVERIFY(!review_resources_only.has_value());
    QCOMPARE(review_resources_only.error().code, ErrorCode::CrossReferenceFailure);

    auto stale_topology = review_pack;
    auto stale_document = stale_topology.resources.front().document;
    auto evidence = stale_document.value(QStringLiteral("evidence")).toObject();
    auto packs = evidence.value(QStringLiteral("packs")).toArray();
    for (qsizetype index = 0; index < packs.size(); ++index) {
        auto binding = packs.at(index).toObject();
        if (binding.value(QStringLiteral("pack_id")).toString() ==
            QStringLiteral("example.subject.detached")) {
            binding.insert(QStringLiteral("dependencies"), QJsonArray{});
            packs.replace(index, binding);
        }
    }
    evidence.insert(QStringLiteral("packs"), packs);
    stale_document.insert(QStringLiteral("evidence"), evidence);
    stale_topology.resources.front().document = stale_document;
    const auto topology_mismatch =
        PackReader::validateResolvedGraph(stale_topology, subject_closure);
    QVERIFY(!topology_mismatch.has_value());
    QCOMPARE(topology_mismatch.error().code, ErrorCode::CrossReferenceFailure);
}

void SchemaDispatchTest::enforcesDetachedIndependentReviewOwnership() {
    validatesManualDetachedReviewCompatibility();

    auto fixture = strictDetachedFixture();
    QVERIFY(fixture.has_value());
    const auto valid = validateStrictDetached(*fixture);
    QVERIFY2(valid.has_value(), valid ? "" : qPrintable(valid.error().message));

    auto schema_one_owner = fixture->detached;
    schema_one_owner.manifest_schema_version = 1;
    refreshPackRevision(schema_one_owner);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &schema_one_owner).has_value());

    auto empty_owner = fixture->detached;
    empty_owner.resources.clear();
    refreshPackRevision(empty_owner);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &empty_owner).has_value());

    auto extra_dependency = fixture->detached;
    extra_dependency.dependencies.push_back(
        appellate::model::PackDependency{fixture->federal.revision});
    refreshPackRevision(extra_dependency);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &extra_dependency).has_value());

    auto wrong_kind = fixture->detached;
    wrong_kind.resources.front().descriptor.kind = appellate::model::ResourceKind::Form;
    refreshPackRevision(wrong_kind);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_kind).has_value());

    auto wrong_schema = fixture->detached;
    wrong_schema.resources.front().descriptor.schema_version = 1;
    refreshPackRevision(wrong_schema);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_schema).has_value());

    auto wrong_id = fixture->detached;
    wrong_id.resources.front().descriptor.id = "test.detached.wrong-id";
    refreshPackRevision(wrong_id);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_id).has_value());

    auto reused_source_id = fixture->detached;
    const auto source_id =
        QString::fromStdString(firstRealismReview(fixture->subject)->descriptor.id);
    reused_source_id.resources.front().descriptor.id = source_id.toStdString();
    reused_source_id.resources.front().document.insert(QStringLiteral("resource_id"), source_id);
    refreshResourceDigest(reused_source_id.resources.front());
    refreshPackRevision(reused_source_id);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &reused_source_id).has_value());

    auto wrong_pin = fixture->detached;
    wrong_pin.dependencies.front().revision.digest = std::string(64, '0');
    const auto wrong_exact_revision = validateStrictDetached(*fixture, nullptr, &wrong_pin);
    QVERIFY(!wrong_exact_revision.has_value());
    QCOMPARE(wrong_exact_revision.error().code, ErrorCode::CrossReferenceFailure);

    auto missing_pin = fixture->detached;
    missing_pin.dependencies.clear();
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &missing_pin).has_value());

    auto transitive_pin = fixture->detached;
    transitive_pin.dependencies = {appellate::model::PackDependency{fixture->ca4.revision}};
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &transitive_pin).has_value());

    auto extra_capability = fixture->detached;
    extra_capability.required_capabilities.push_back(
        appellate::model::RequiredCapability{"workbench.pack.canonical-authority", 1});
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &extra_capability).has_value());

    auto missing_capability = fixture->detached;
    missing_capability.required_capabilities.pop_back();
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &missing_capability).has_value());

    auto reordered_capabilities = fixture->detached;
    std::ranges::reverse(reordered_capabilities.required_capabilities);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &reordered_capabilities).has_value());

    auto wrong_path = fixture->detached;
    wrong_path.resources.front().descriptor.path = "resources/other-review.json";
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_path).has_value());

    auto wrong_descriptor_digest = fixture->detached;
    wrong_descriptor_digest.resources.front().descriptor.sha256 = std::string(64, '0');
    refreshPackRevision(wrong_descriptor_digest);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_descriptor_digest).has_value());

    auto extra_resource = fixture->detached;
    auto duplicate = extra_resource.resources.front();
    duplicate.descriptor.id = "test.detached.extra-review";
    duplicate.document.insert(QStringLiteral("resource_id"),
                              QStringLiteral("test.detached.extra-review"));
    extra_resource.resources.push_back(std::move(duplicate));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &extra_resource).has_value());

    auto with_blob = fixture->detached;
    with_blob.blobs.push_back(appellate::model::BlobDescriptor{
        "objects/test.pdf", "application/pdf", 1, std::string(64, '1')});
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &with_blob).has_value());

    auto wrong_state = fixture->detached;
    wrong_state.resources.front().document.insert(QStringLiteral("review_state"),
                                                  QStringLiteral("independent_review_pending"));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_state).has_value());

    auto mixed_trace_profile = fixture->detached;
    auto mixed_document = mixed_trace_profile.resources.front().document;
    auto mixed_evidence = mixed_document.value(QStringLiteral("evidence")).toObject();
    auto mixed_traces = mixed_evidence.value(QStringLiteral("traces")).toArray();
    auto mixed_trace = mixed_traces.first().toObject();
    mixed_trace.insert(QStringLiteral("engine_revision"),
                       QStringLiteral("appellate.realism-evidence.codec-replay-multi.v1"));
    mixed_trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(mixed_document.value(QStringLiteral("case_id")).toString(),
                           mixed_trace));
    mixed_traces.replace(0, mixed_trace);
    mixed_evidence.insert(QStringLiteral("traces"), mixed_traces);
    mixed_document.insert(QStringLiteral("evidence"), mixed_evidence);
    mixed_trace_profile.resources.front().document = mixed_document;
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &mixed_trace_profile).has_value());

    auto stale_trace = fixture->detached;
    auto stale_document = stale_trace.resources.front().document;
    auto stale_evidence = stale_document.value(QStringLiteral("evidence")).toObject();
    auto stale_traces = stale_evidence.value(QStringLiteral("traces")).toArray();
    auto stale_first = stale_traces.first().toObject();
    stale_first.insert(QStringLiteral("terminal_stage_id"), QStringLiteral("test.stage.other"));
    stale_first.insert(
        QStringLiteral("digest"),
        realismTraceDigest(stale_document.value(QStringLiteral("case_id")).toString(),
                           stale_first));
    stale_traces.replace(0, stale_first);
    stale_evidence.insert(QStringLiteral("traces"), stale_traces);
    stale_document.insert(QStringLiteral("evidence"), stale_evidence);
    stale_trace.resources.front().document = stale_document;
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &stale_trace).has_value());

    auto stale_operation = fixture->detached;
    auto stale_operation_document = stale_operation.resources.front().document;
    auto stale_operation_evidence =
        stale_operation_document.value(QStringLiteral("evidence")).toObject();
    auto stale_operation_traces =
        stale_operation_evidence.value(QStringLiteral("traces")).toArray();
    auto stale_operation_trace = stale_operation_traces.first().toObject();
    auto operation_ids = stale_operation_trace.value(QStringLiteral("operation_ids")).toArray();
    operation_ids.replace(0, QStringLiteral("test.detached.operation.wrong"));
    stale_operation_trace.insert(QStringLiteral("operation_ids"), operation_ids);
    stale_operation_traces.replace(0, stale_operation_trace);
    stale_operation_evidence.insert(QStringLiteral("traces"), stale_operation_traces);
    stale_operation_document.insert(QStringLiteral("evidence"), stale_operation_evidence);
    stale_operation.resources.front().document = stale_operation_document;
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &stale_operation).has_value());

    auto earlier_date = fixture->detached;
    earlier_date.resources.front().document.insert(QStringLiteral("reviewed_on"),
                                                   QStringLiteral("2026-08-18"));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &earlier_date).has_value());

    auto no_source_review = fixture->subject;
    std::erase_if(no_source_review.resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    QVERIFY(!validateStrictDetached(*fixture, &no_source_review).has_value());

    auto duplicate_source_review = fixture->subject;
    auto* source_review = firstRealismReview(duplicate_source_review);
    QVERIFY(source_review != nullptr);
    auto second_source = *source_review;
    second_source.descriptor.id = "test.source.second-review";
    second_source.document.insert(QStringLiteral("resource_id"),
                                  QStringLiteral("test.source.second-review"));
    duplicate_source_review.resources.push_back(std::move(second_source));
    QVERIFY(!validateStrictDetached(*fixture, &duplicate_source_review).has_value());

    auto different_case_positive = *fixture;
    const auto original_case =
        std::ranges::find_if(different_case_positive.subject.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::Case;
        });
    const auto original_argument =
        std::ranges::find_if(different_case_positive.subject.resources, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::ArgumentConfig;
        });
    QVERIFY(original_case != different_case_positive.subject.resources.end());
    QVERIFY(original_argument != different_case_positive.subject.resources.end());
    const auto different_case_id = QStringLiteral("aa.test.detached.case.different");
    auto cloned_case = *original_case;
    cloned_case.descriptor.id = different_case_id.toStdString();
    cloned_case.descriptor.path = "resources/test-different-case.json";
    cloned_case.document.insert(QStringLiteral("resource_id"), different_case_id);
    refreshDispositionDigests(cloned_case.document);
    refreshResourceDigest(cloned_case);
    auto cloned_argument = *original_argument;
    const auto cloned_argument_id = QStringLiteral("test.detached.argument.different");
    cloned_argument.descriptor.id = cloned_argument_id.toStdString();
    cloned_argument.descriptor.path = "resources/test-different-argument.json";
    cloned_argument.document.insert(QStringLiteral("resource_id"), cloned_argument_id);
    cloned_argument.document.insert(QStringLiteral("case_id"), different_case_id);
    cloned_argument.document.remove(QStringLiteral("grounded_question_bank"));
    refreshResourceDigest(cloned_argument);
    different_case_positive.subject.resources.insert(
        different_case_positive.subject.resources.begin(), std::move(cloned_argument));
    different_case_positive.subject.resources.insert(
        different_case_positive.subject.resources.begin(), std::move(cloned_case));
    rebuildStrictDetachedSubjectClosure(different_case_positive, {});

    const auto different_review_id = QStringLiteral("test.detached.review.different-case");
    appellate::packs::ValidatedResource different_case_review{
        appellate::model::DeclarativeResource{
            appellate::model::ResourceKind::RealismReview,
            different_review_id.toStdString(),
            2,
            "resources/test-different-realism-review.json",
            {},
        },
        QJsonObject{
            {QStringLiteral("schema_version"), 2},
            {QStringLiteral("resource_kind"), QStringLiteral("realism_review")},
            {QStringLiteral("resource_id"), different_review_id},
            {QStringLiteral("case_id"), different_case_id},
            {QStringLiteral("review_state"), QStringLiteral("self_reviewed")},
            {QStringLiteral("dimensions"),
             QJsonObject{
                 {QStringLiteral("procedural_law"), 2},
                 {QStringLiteral("deadlines_authority"), 2},
                 {QStringLiteral("record_consistency"), 2},
                 {QStringLiteral("consequences"), 2},
                 {QStringLiteral("oral_argument"), 2},
                 {QStringLiteral("bench_differentiation"), 2},
                 {QStringLiteral("provenance"), 2},
             }},
            {QStringLiteral("known_uncertainty"), QJsonArray{}},
        },
    };
    const std::array<const appellate::packs::LoadedPack*, 4> replay_dependencies{
        &different_case_positive.federal, &different_case_positive.ca4,
        &different_case_positive.bench, &different_case_positive.subject};
    const auto replay_runtime = appellate::packs::loadRuntimePackForEvidence(
        different_case_positive.subject, replay_dependencies, true);
    QVERIFY2(replay_runtime.has_value(),
             replay_runtime ? "" : replay_runtime.error().message.c_str());
    QVERIFY(!replay_runtime->cases.empty());
    QCOMPARE(QString::fromStdString(replay_runtime->cases.front().definition.id.value),
             different_case_id);
    const auto source_trace = firstRealismReview(different_case_positive.subject)
                                  ->document.value(QStringLiteral("evidence"))
                                  .toObject()
                                  .value(QStringLiteral("traces"))
                                  .toArray()
                                  .first()
                                  .toObject();
    const auto different_trace_value =
        replayFirstTraceCommand(source_trace, replay_runtime->cases.front(), different_case_id);
    QVERIFY(different_trace_value.has_value());
    auto different_trace = *different_trace_value;
    different_case_review.document.insert(
        QStringLiteral("evidence"),
        QJsonObject{{QStringLiteral("traces"), QJsonArray{different_trace}}});
    const std::array<const appellate::packs::LoadedPack*, 4> different_subject_closure{
        &different_case_positive.federal, &different_case_positive.ca4,
        &different_case_positive.bench, &different_case_positive.subject};
    const auto different_review_document = buildRealismReview(
        different_case_review, different_case_positive.subject, different_subject_closure, false);
    QVERIFY(different_review_document.has_value());
    different_case_review.document = *different_review_document;
    auto different_evidence =
        different_case_review.document.value(QStringLiteral("evidence")).toObject();
    auto different_authorities = different_evidence.value(QStringLiteral("authorities")).toArray();
    QString first_authority_evidence_id;
    for (qsizetype index = 0; index < different_authorities.size(); ++index) {
        auto authority = different_authorities.at(index).toObject();
        const auto evidence_id =
            QStringLiteral("test.detached.evidence.different-authority.%1").arg(index);
        authority.insert(QStringLiteral("evidence_id"), evidence_id);
        different_authorities.replace(index, authority);
        if (index == 0) {
            first_authority_evidence_id = evidence_id;
        }
    }
    QVERIFY(!first_authority_evidence_id.isEmpty());
    auto different_groups =
        different_evidence.value(QStringLiteral("dimension_evidence")).toObject();
    for (const auto& key : different_groups.keys()) {
        auto references = different_groups.value(key).toArray();
        for (qsizetype index = 0; index < references.size(); ++index) {
            if (references.at(index).toString() == QStringLiteral("example.evidence.authority-1")) {
                references.replace(index, first_authority_evidence_id);
            } else if (references.at(index).toString() ==
                       QStringLiteral("example.evidence.trace-1")) {
                references.replace(index,
                                   different_trace.value(QStringLiteral("evidence_id")).toString());
            }
        }
        different_groups.insert(key, references);
    }
    different_evidence.insert(QStringLiteral("authorities"), different_authorities);
    different_evidence.insert(QStringLiteral("dimension_evidence"), different_groups);
    different_case_review.document.insert(QStringLiteral("evidence"), different_evidence);
    refreshResourceDigest(different_case_review);
    different_case_positive.subject.resources.push_back(std::move(different_case_review));
    refreshStrictDetachedFixture(different_case_positive);
    const std::array<const appellate::packs::LoadedPack*, 3> different_dependencies{
        &different_case_positive.federal, &different_case_positive.ca4,
        &different_case_positive.bench};
    const auto different_subject_result =
        PackReader::validateResolvedGraph(different_case_positive.subject, different_dependencies);
    QVERIFY2(different_subject_result.has_value(),
             different_subject_result ? "" : qPrintable(different_subject_result.error().message));
    const auto different_detached_result = validateStrictDetached(different_case_positive);
    QVERIFY2(different_detached_result.has_value(),
             different_detached_result ? ""
                                       : qPrintable(different_detached_result.error().message));

    auto different_case_only = different_case_positive;
    const auto reviewed_case_id =
        fixture->detached.resources.front().document.value(QStringLiteral("case_id")).toString();
    std::erase_if(different_case_only.subject.resources, [&](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::RealismReview &&
               resource.document.value(QStringLiteral("case_id")).toString() == reviewed_case_id;
    });
    refreshPackRevision(different_case_only.subject);
    different_case_only.detached.dependencies = {
        appellate::model::PackDependency{different_case_only.subject.revision}};
    refreshPackRevision(different_case_only.detached);
    const auto different_case_only_subject =
        PackReader::validateResolvedGraph(different_case_only.subject, different_dependencies);
    QVERIFY2(different_case_only_subject.has_value(),
             different_case_only_subject ? ""
                                         : qPrintable(different_case_only_subject.error().message));
    QVERIFY(!validateStrictDetached(different_case_only).has_value());

    const auto validate_subject = [](const StrictDetachedFixture& candidate,
                                     const appellate::packs::LoadedPack& subject) {
        const std::array<const appellate::packs::LoadedPack*, 3> dependencies{
            &candidate.federal, &candidate.ca4, &candidate.bench};
        return PackReader::validateResolvedGraph(subject, dependencies);
    };

    auto source_wrong_state = *fixture;
    auto* changed_source = firstRealismReview(source_wrong_state.subject);
    QVERIFY(changed_source != nullptr);
    changed_source->document.insert(QStringLiteral("review_state"),
                                    QStringLiteral("self_reviewed"));
    refreshStrictDetachedFixture(source_wrong_state);
    QVERIFY(validate_subject(source_wrong_state, source_wrong_state.subject).has_value());
    QVERIFY(!validateStrictDetached(source_wrong_state).has_value());

    auto source_wrong_profile = *fixture;
    changed_source = firstRealismReview(source_wrong_profile.subject);
    QVERIFY(changed_source != nullptr);
    useManualTraceProfile(*changed_source);
    refreshStrictDetachedFixture(source_wrong_profile);
    QVERIFY(validate_subject(source_wrong_profile, source_wrong_profile.subject).has_value());
    QVERIFY(!validateStrictDetached(source_wrong_profile).has_value());

    const auto add_trace_reference = [](appellate::packs::ValidatedResource& review,
                                        const QString& existing_id, const QString& added_id) {
        auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
        auto groups = evidence.value(QStringLiteral("dimension_evidence")).toObject();
        for (const auto& key : groups.keys()) {
            auto references = groups.value(key).toArray();
            if (references.contains(existing_id)) {
                references.push_back(added_id);
                groups.insert(key, references);
            }
        }
        evidence.insert(QStringLiteral("dimension_evidence"), groups);
        review.document.insert(QStringLiteral("evidence"), evidence);
    };
    const auto remove_trace_reference = [](appellate::packs::ValidatedResource& review,
                                           const QString& removed_id) {
        auto evidence = review.document.value(QStringLiteral("evidence")).toObject();
        auto groups = evidence.value(QStringLiteral("dimension_evidence")).toObject();
        for (const auto& key : groups.keys()) {
            QJsonArray retained;
            for (const auto& value : groups.value(key).toArray()) {
                if (value.toString() != removed_id) {
                    retained.push_back(value);
                }
            }
            groups.insert(key, retained);
        }
        evidence.insert(QStringLiteral("dimension_evidence"), groups);
        review.document.insert(QStringLiteral("evidence"), evidence);
    };

    auto added_source_trace = *fixture;
    changed_source = firstRealismReview(added_source_trace.subject);
    QVERIFY(changed_source != nullptr);
    auto changed_evidence = changed_source->document.value(QStringLiteral("evidence")).toObject();
    auto changed_traces = changed_evidence.value(QStringLiteral("traces")).toArray();
    auto added_trace = changed_traces.last().toObject();
    const auto copied_trace_id = added_trace.value(QStringLiteral("evidence_id")).toString();
    const auto added_trace_id = QStringLiteral("test.detached.evidence.added-trace");
    added_trace.insert(QStringLiteral("evidence_id"), added_trace_id);
    added_trace.insert(QStringLiteral("trace_id"),
                       QStringLiteral("test.detached.trace.added-trace"));
    added_trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(changed_source->document.value(QStringLiteral("case_id")).toString(),
                           added_trace));
    changed_traces.push_back(added_trace);
    changed_evidence.insert(QStringLiteral("traces"), changed_traces);
    changed_source->document.insert(QStringLiteral("evidence"), changed_evidence);
    add_trace_reference(*changed_source, copied_trace_id, added_trace_id);
    refreshStrictDetachedFixture(added_source_trace);
    QVERIFY(validate_subject(added_source_trace, added_source_trace.subject).has_value());
    QVERIFY(!validateStrictDetached(added_source_trace).has_value());

    auto dropped_source_trace = *fixture;
    changed_source = firstRealismReview(dropped_source_trace.subject);
    QVERIFY(changed_source != nullptr);
    changed_evidence = changed_source->document.value(QStringLiteral("evidence")).toObject();
    changed_traces = changed_evidence.value(QStringLiteral("traces")).toArray();
    const auto dropped_trace_id =
        changed_traces.first().toObject().value(QStringLiteral("evidence_id")).toString();
    changed_traces.removeFirst();
    changed_evidence.insert(QStringLiteral("traces"), changed_traces);
    changed_source->document.insert(QStringLiteral("evidence"), changed_evidence);
    remove_trace_reference(*changed_source, dropped_trace_id);
    refreshStrictDetachedFixture(dropped_source_trace);
    QVERIFY(validate_subject(dropped_source_trace, dropped_source_trace.subject).has_value());
    QVERIFY(!validateStrictDetached(dropped_source_trace).has_value());

    auto reordered_source_traces = *fixture;
    changed_source = firstRealismReview(reordered_source_traces.subject);
    QVERIFY(changed_source != nullptr);
    changed_evidence = changed_source->document.value(QStringLiteral("evidence")).toObject();
    changed_traces = changed_evidence.value(QStringLiteral("traces")).toArray();
    QJsonArray reversed_traces;
    for (qsizetype index = changed_traces.size(); index > 0; --index) {
        reversed_traces.push_back(changed_traces.at(index - 1));
    }
    changed_traces = reversed_traces;
    changed_evidence.insert(QStringLiteral("traces"), changed_traces);
    changed_source->document.insert(QStringLiteral("evidence"), changed_evidence);
    refreshStrictDetachedFixture(reordered_source_traces);
    QVERIFY(validate_subject(reordered_source_traces, reordered_source_traces.subject).has_value());
    QVERIFY(!validateStrictDetached(reordered_source_traces).has_value());

    auto changed_source_field = *fixture;
    changed_source = firstRealismReview(changed_source_field.subject);
    QVERIFY(changed_source != nullptr);
    changed_evidence = changed_source->document.value(QStringLiteral("evidence")).toObject();
    changed_traces = changed_evidence.value(QStringLiteral("traces")).toArray();
    auto changed_trace = changed_traces.last().toObject();
    const auto old_evidence_id = changed_trace.value(QStringLiteral("evidence_id")).toString();
    const auto new_evidence_id = QStringLiteral("test.detached.evidence.changed-field");
    changed_trace.insert(QStringLiteral("evidence_id"), new_evidence_id);
    changed_trace.insert(
        QStringLiteral("digest"),
        realismTraceDigest(changed_source->document.value(QStringLiteral("case_id")).toString(),
                           changed_trace));
    changed_traces.replace(changed_traces.size() - 1, changed_trace);
    changed_evidence.insert(QStringLiteral("traces"), changed_traces);
    changed_source->document.insert(QStringLiteral("evidence"), changed_evidence);
    remove_trace_reference(*changed_source, old_evidence_id);
    add_trace_reference(
        *changed_source,
        changed_traces.first().toObject().value(QStringLiteral("evidence_id")).toString(),
        new_evidence_id);
    refreshStrictDetachedFixture(changed_source_field);
    QVERIFY(validate_subject(changed_source_field, changed_source_field.subject).has_value());
    QVERIFY(!validateStrictDetached(changed_source_field).has_value());

    auto changed_source_journal = *fixture;
    changed_source = firstRealismReview(changed_source_journal.subject);
    QVERIFY(changed_source != nullptr);
    QVERIFY(mutateFirstTraceCommandField(*changed_source,
                                         QStringLiteral("TEST-ONLY changed source journal")));
    refreshStrictDetachedFixture(changed_source_journal);
    QVERIFY(validate_subject(changed_source_journal, changed_source_journal.subject).has_value());
    QVERIFY(!validateStrictDetached(changed_source_journal).has_value());

    auto changed_closure = fixture->detached;
    auto changed_closure_document = changed_closure.resources.front().document;
    auto changed_closure_evidence =
        changed_closure_document.value(QStringLiteral("evidence")).toObject();
    changed_closure_evidence.insert(QStringLiteral("closure_digest"), QString(64, u'0'));
    changed_closure_document.insert(QStringLiteral("evidence"), changed_closure_evidence);
    changed_closure.resources.front().document = changed_closure_document;
    refreshResourceDigest(changed_closure.resources.front());
    refreshPackRevision(changed_closure);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &changed_closure).has_value());

    for (const auto& array_name :
         {QStringLiteral("resources"), QStringLiteral("blobs"), QStringLiteral("authorities"),
          QStringLiteral("record_checks")}) {
        auto changed_binding = fixture->detached;
        auto document = changed_binding.resources.front().document;
        auto binding_evidence = document.value(QStringLiteral("evidence")).toObject();
        auto values = binding_evidence.value(array_name).toArray();
        QVERIFY(!values.isEmpty());
        auto value = values.first().toObject();
        const auto binding_old_evidence_id = value.value(QStringLiteral("evidence_id")).toString();
        QString replacement_evidence_id;
        if (array_name == QStringLiteral("authorities")) {
            replacement_evidence_id = QStringLiteral("test.detached.evidence.wrong-authority");
            value.insert(QStringLiteral("evidence_id"), replacement_evidence_id);
        } else if (array_name == QStringLiteral("record_checks")) {
            value.insert(QStringLiteral("digest"), QString(64, u'0'));
        } else if (array_name == QStringLiteral("resources")) {
            replacement_evidence_id = QStringLiteral("test.detached.evidence.wrong-resource");
            value.insert(QStringLiteral("evidence_id"), replacement_evidence_id);
        } else {
            value.insert(QStringLiteral("sha256"), QString(64, u'0'));
        }
        values.replace(0, value);
        binding_evidence.insert(array_name, values);
        if (!replacement_evidence_id.isEmpty()) {
            auto groups = binding_evidence.value(QStringLiteral("dimension_evidence")).toObject();
            for (const auto& key : groups.keys()) {
                auto references = groups.value(key).toArray();
                for (qsizetype index = 0; index < references.size(); ++index) {
                    if (references.at(index).toString() == binding_old_evidence_id) {
                        references.replace(index, replacement_evidence_id);
                    }
                }
                groups.insert(key, references);
            }
            binding_evidence.insert(QStringLiteral("dimension_evidence"), groups);
        }
        if (array_name == QStringLiteral("resources")) {
            binding_evidence.insert(
                QStringLiteral("closure_digest"),
                realismClosureDigest(document.value(QStringLiteral("case_id")).toString(),
                                     binding_evidence.value(QStringLiteral("packs")).toArray(),
                                     values,
                                     binding_evidence.value(QStringLiteral("blobs")).toArray()));
        }
        document.insert(QStringLiteral("evidence"), binding_evidence);
        changed_binding.resources.front().document = document;
        refreshResourceDigest(changed_binding.resources.front());
        refreshPackRevision(changed_binding);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &changed_binding).has_value());
    }

    auto changed_partition = fixture->detached;
    auto changed_partition_document = changed_partition.resources.front().document;
    auto changed_partition_evidence =
        changed_partition_document.value(QStringLiteral("evidence")).toObject();
    auto changed_groups =
        changed_partition_evidence.value(QStringLiteral("dimension_evidence")).toObject();
    auto deadline_references =
        changed_groups.value(QStringLiteral("deadlines_authority")).toArray();
    QString unrelated_reference;
    for (const auto& value :
         changed_partition_evidence.value(QStringLiteral("resources")).toArray()) {
        const auto candidate = value.toObject().value(QStringLiteral("evidence_id")).toString();
        if (!deadline_references.contains(candidate)) {
            unrelated_reference = candidate;
            break;
        }
    }
    QVERIFY(!unrelated_reference.isEmpty());
    deadline_references.push_back(unrelated_reference);
    changed_groups.insert(QStringLiteral("deadlines_authority"), deadline_references);
    changed_partition_evidence.insert(QStringLiteral("dimension_evidence"), changed_groups);
    changed_partition_document.insert(QStringLiteral("evidence"), changed_partition_evidence);
    changed_partition.resources.front().document = changed_partition_document;
    refreshResourceDigest(changed_partition.resources.front());
    refreshPackRevision(changed_partition);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &changed_partition).has_value());

    const auto exercise_revision_boundary = [&](int filler_count, bool expect_success) {
        auto candidate = *fixture;
        std::vector<appellate::packs::LoadedPack> fillers;
        fillers.reserve(static_cast<std::size_t>(filler_count));
        for (int index = 0; index < filler_count; ++index) {
            appellate::packs::LoadedPack filler{
                2,
                appellate::model::PackRevision{
                    appellate::model::PackId{
                        QStringLiteral("test.detached.boundary-pack.%1").arg(index).toStdString()},
                    "1.0.0",
                    {}},
                {appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2}},
                {},
                {},
                {},
                {},
                appellate::packs::PackGraphState::StandaloneValidated,
            };
            refreshPackRevision(filler);
            fillers.push_back(std::move(filler));
        }
        std::vector<const appellate::packs::LoadedPack*> additional;
        additional.reserve(fillers.size());
        for (const auto& filler : fillers) {
            additional.push_back(&filler);
            candidate.subject.dependencies.push_back(
                appellate::model::PackDependency{filler.revision});
        }
        rebuildStrictDetachedSubjectClosure(candidate, additional);
        QCOMPARE(additional.size() + 4U, static_cast<std::size_t>(filler_count + 4));
        const auto result = validateStrictDetached(candidate, nullptr, nullptr, additional);
        if (expect_success) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        } else {
            QVERIFY(!result.has_value());
        }
    };
    exercise_revision_boundary(123, true);
    exercise_revision_boundary(124, false);

    const auto exercise_descriptor_boundary = [&](std::size_t target_count, bool expect_success) {
        auto candidate = *fixture;
        const auto baseline_count =
            candidate.federal.resources.size() + candidate.federal.blobs.size() +
            candidate.ca4.resources.size() + candidate.ca4.blobs.size() +
            candidate.bench.resources.size() + candidate.bench.blobs.size() +
            candidate.subject.resources.size() + candidate.subject.blobs.size();
        QVERIFY(target_count >= baseline_count);
        const auto filler_count = target_count - baseline_count;
        appellate::packs::LoadedPack filler{
            2,
            appellate::model::PackRevision{
                appellate::model::PackId{"test.detached.descriptor-boundary"}, "1.0.0", {}},
            {
                appellate::model::RequiredCapability{"workbench.pack.declarative-resources", 2},
                appellate::model::RequiredCapability{"workbench.pack.judge-profile", 2},
                appellate::model::RequiredCapability{"workbench.pack.voice-style", 2},
            },
            {},
            {},
            {},
            {},
            appellate::packs::PackGraphState::StandaloneValidated,
        };
        const appellate::packs::ValidatedResource* judge_prototype = nullptr;
        const appellate::model::JudgeProfile* typed_judge_prototype = nullptr;
        for (const auto* pack :
             {&candidate.federal, &candidate.ca4, &candidate.bench, &candidate.subject}) {
            const auto judge = std::ranges::find_if(pack->resources, [](const auto& resource) {
                return resource.descriptor.kind == appellate::model::ResourceKind::JudgeProfile;
            });
            if (judge != pack->resources.end()) {
                judge_prototype = &*judge;
                const auto typed =
                    std::ranges::find(pack->judge_profiles, judge->descriptor.id,
                                      [](const auto& profile) { return profile.id; });
                if (typed != pack->judge_profiles.end()) {
                    typed_judge_prototype = &*typed;
                }
                break;
            }
        }
        QVERIFY(judge_prototype != nullptr);
        QVERIFY(typed_judge_prototype != nullptr);
        filler.resources.reserve(filler_count);
        filler.judge_profiles.reserve(filler_count);
        for (std::size_t index = 0; index < filler_count; ++index) {
            auto judge = *judge_prototype;
            const auto resource_id =
                QStringLiteral("test.detached.judge.%1").arg(index, 5, 10, QChar{u'0'});
            judge.descriptor.id = resource_id.toStdString();
            judge.descriptor.path = QStringLiteral("resources/filler-judge-%1.json")
                                        .arg(index, 5, 10, QChar{u'0'})
                                        .toStdString();
            judge.document.insert(QStringLiteral("resource_id"), resource_id);
            refreshResourceDigest(judge);
            filler.resources.push_back(std::move(judge));
            auto typed_judge = *typed_judge_prototype;
            typed_judge.id = resource_id.toStdString();
            filler.judge_profiles.push_back(std::move(typed_judge));
        }
        refreshPackRevision(filler);
        candidate.subject.dependencies.push_back(appellate::model::PackDependency{filler.revision});
        const std::array<const appellate::packs::LoadedPack*, 1> additional{&filler};
        rebuildStrictDetachedSubjectClosure(candidate, additional);
        QCOMPARE(baseline_count + filler.resources.size(), target_count);
        const auto result = validateStrictDetached(candidate, nullptr, nullptr, additional);
        if (expect_success) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        } else {
            QVERIFY(!result.has_value());
        }
    };
    exercise_descriptor_boundary(9'999, true);
    exercise_descriptor_boundary(10'000, false);

    const auto exercise_latent_boundary = [&](qsizetype target_size, bool expect_success) {
        auto candidate = *fixture;
        auto authority_set =
            std::ranges::find_if(candidate.subject.resources, [](const auto& item) {
                return item.descriptor.kind == appellate::model::ResourceKind::AuthoritySet;
            });
        auto record = std::ranges::find_if(candidate.subject.resources, [](const auto& item) {
            return item.descriptor.kind == appellate::model::ResourceKind::Record;
        });
        QVERIFY(authority_set != candidate.subject.resources.end());
        QVERIFY(record != candidate.subject.resources.end());
        auto authorities = authority_set->document.value(QStringLiteral("authorities")).toArray();
        QVERIFY(!authorities.isEmpty());
        const auto prototype = authorities.first().toObject();

        auto* source = firstRealismReview(candidate.subject);
        QVERIFY(source != nullptr);
        auto source_evidence = source->document.value(QStringLiteral("evidence")).toObject();
        auto source_groups = source_evidence.value(QStringLiteral("dimension_evidence")).toObject();
        auto provenance = source_groups.value(QStringLiteral("provenance")).toArray();
        QVERIFY(target_size >= provenance.size());
        auto authority_bindings = source_evidence.value(QStringLiteral("authorities")).toArray();
        QJsonArray record_authority_ids;
        const auto case_id = source->document.value(QStringLiteral("case_id")).toString();
        for (qsizetype index = provenance.size(); index < target_size; ++index) {
            const auto authority_id = QStringLiteral("zztest.detached.authority.%1").arg(index);
            auto authority = prototype;
            authority.insert(QStringLiteral("authority_id"), authority_id);
            authorities.push_back(authority);
            record_authority_ids.push_back(authority_id);
            const auto evidence_id =
                codeOwnedEvidenceId(QStringLiteral("authority"), {case_id, authority_id});
            authority_bindings.push_back(QJsonObject{
                {QStringLiteral("evidence_id"), evidence_id},
                {QStringLiteral("authority_id"), authority_id},
            });
            provenance.push_back(evidence_id);
        }
        authority_set->document.insert(QStringLiteral("authorities"), authorities);
        record->document.insert(QStringLiteral("authority_ids"), record_authority_ids);
        auto provenance_values = provenance.toVariantList();
        std::ranges::sort(provenance_values, {},
                          [](const QVariant& value) { return value.toString(); });
        provenance = QJsonArray::fromVariantList(provenance_values);
        source_groups.insert(QStringLiteral("provenance"), QJsonArray{});
        source_evidence.insert(QStringLiteral("dimension_evidence"), source_groups);
        source_evidence.insert(QStringLiteral("authorities"), authority_bindings);
        source->document.insert(QStringLiteral("evidence"), source_evidence);
        auto source_dimensions = source->document.value(QStringLiteral("dimensions")).toObject();
        source_dimensions.insert(QStringLiteral("provenance"), 0);
        source->document.insert(QStringLiteral("dimensions"), source_dimensions);

        auto detached_evidence = candidate.detached.resources.front()
                                     .document.value(QStringLiteral("evidence"))
                                     .toObject();
        detached_evidence.insert(QStringLiteral("dimension_evidence"), source_groups);
        detached_evidence.insert(QStringLiteral("authorities"), authority_bindings);
        candidate.detached.resources.front().document.insert(QStringLiteral("evidence"),
                                                             detached_evidence);
        auto detached_dimensions = candidate.detached.resources.front()
                                       .document.value(QStringLiteral("dimensions"))
                                       .toObject();
        detached_dimensions.insert(QStringLiteral("provenance"), 0);
        candidate.detached.resources.front().document.insert(QStringLiteral("dimensions"),
                                                             detached_dimensions);
        refreshStrictDetachedFixture(candidate);
        QCOMPARE(provenance.size(), target_size);
        const std::array<const appellate::packs::LoadedPack*, 3> dependencies{
            &candidate.federal, &candidate.ca4, &candidate.bench};
        const auto source_result =
            PackReader::validateResolvedGraph(candidate.subject, dependencies);
        QVERIFY2(source_result.has_value(),
                 source_result ? "" : qPrintable(source_result.error().message));
        const auto result = validateStrictDetached(candidate);
        if (expect_success) {
            QVERIFY2(result.has_value(), result ? "" : qPrintable(result.error().message));
        } else {
            QVERIFY(!result.has_value());
        }
    };
    exercise_latent_boundary(512, true);
    exercise_latent_boundary(513, false);
}

void SchemaDispatchTest::enforcesDetachedIndependentReviewHumanShape() {
    auto fixture = strictDetachedFixture();
    QVERIFY(fixture.has_value());
    const auto multibyte_boundary = [](qsizetype maximum_bytes, bool overflow) {
        QString value(static_cast<qsizetype>(maximum_bytes / 2 - 1), QChar{0x00E9});
        value.append(QString(overflow ? 3 : 2, u'x'));
        return value;
    };
    const auto with_reviewer_field = [](const appellate::packs::LoadedPack& source,
                                        const QString& key, const QJsonValue& value) {
        auto result = source;
        auto reviewer =
            result.resources.front().document.value(QStringLiteral("reviewer")).toObject();
        reviewer.insert(key, value);
        result.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
        return result;
    };
    const auto with_uncertainty_field = [](const appellate::packs::LoadedPack& source,
                                           const QString& key, const QJsonValue& value) {
        auto result = source;
        auto uncertainties =
            result.resources.front().document.value(QStringLiteral("known_uncertainty")).toArray();
        auto uncertainty = uncertainties.first().toObject();
        uncertainty.insert(key, value);
        uncertainties.replace(0, uncertainty);
        result.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                 uncertainties);
        return result;
    };

    auto affiliation = fixture->detached;
    auto affiliation_reviewer =
        affiliation.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    affiliation_reviewer.insert(QStringLiteral("affiliation"),
                                QStringLiteral("TEST-ONLY affiliation"));
    affiliation.resources.front().document.insert(QStringLiteral("reviewer"), affiliation_reviewer);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &affiliation).has_value());

    auto exact_reference = fixture->detached;
    exact_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                      QString(512, u'r'));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &exact_reference).has_value());
    exact_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                      QString(513, u'r'));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &exact_reference).has_value());

    auto multibyte_reference = fixture->detached;
    multibyte_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                          multibyte_boundary(512, false));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &multibyte_reference).has_value());
    multibyte_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                          multibyte_boundary(512, true));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &multibyte_reference).has_value());

    auto empty_reference = fixture->detached;
    empty_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                      QString{});
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &empty_reference).has_value());

    auto whitespace_reference = fixture->detached;
    whitespace_reference.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                           QStringLiteral(" leading whitespace"));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &whitespace_reference).has_value());

    for (const auto& invalid_reference :
         {QStringLiteral("trailing whitespace "), QStringLiteral(" \t\n")}) {
        auto candidate = fixture->detached;
        candidate.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                    invalid_reference);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &candidate).has_value());
    }
    for (const auto& invalid_reference : {QJsonValue{QJsonValue::Undefined}, QJsonValue{7}}) {
        auto candidate = fixture->detached;
        if (invalid_reference.isUndefined()) {
            candidate.resources.front().document.remove(QStringLiteral("reviewer_reference"));
        } else {
            candidate.resources.front().document.insert(QStringLiteral("reviewer_reference"),
                                                        invalid_reference);
        }
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &candidate).has_value());
    }

    auto overlong_name = fixture->detached;
    auto reviewer =
        overlong_name.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("display_name"), QString(241, u'n'));
    overlong_name.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &overlong_name).has_value());

    auto display_boundary = with_reviewer_field(fixture->detached, QStringLiteral("display_name"),
                                                multibyte_boundary(240, false));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &display_boundary).has_value());
    display_boundary = with_reviewer_field(fixture->detached, QStringLiteral("display_name"),
                                           multibyte_boundary(240, true));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &display_boundary).has_value());

    auto qualification_boundary = with_reviewer_field(
        fixture->detached, QStringLiteral("qualification"), multibyte_boundary(1'024, false));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &qualification_boundary).has_value());
    qualification_boundary = with_reviewer_field(fixture->detached, QStringLiteral("qualification"),
                                                 multibyte_boundary(1'024, true));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &qualification_boundary).has_value());

    auto affiliation_boundary = with_reviewer_field(
        fixture->detached, QStringLiteral("affiliation"), multibyte_boundary(240, false));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &affiliation_boundary).has_value());
    affiliation_boundary = with_reviewer_field(fixture->detached, QStringLiteral("affiliation"),
                                               multibyte_boundary(240, true));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &affiliation_boundary).has_value());

    for (const auto& field : {QStringLiteral("display_name"), QStringLiteral("qualification"),
                              QStringLiteral("affiliation")}) {
        for (const auto& invalid_text : {QString{}, QStringLiteral(" padded"),
                                         QStringLiteral("padded "), QStringLiteral(" \t\n")}) {
            const auto invalid = with_reviewer_field(fixture->detached, field, invalid_text);
            QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid).has_value());
        }
    }

    auto invalid_reviewer_id = with_reviewer_field(fixture->detached, QStringLiteral("reviewer_id"),
                                                   QStringLiteral("Invalid Reviewer"));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid_reviewer_id).has_value());

    auto reviewer_id_boundary = with_reviewer_field(
        fixture->detached, QStringLiteral("reviewer_id"), QStringLiteral("a.b"));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &reviewer_id_boundary).has_value());
    reviewer_id_boundary = with_reviewer_field(fixture->detached, QStringLiteral("reviewer_id"),
                                               QStringLiteral("a.") + QString(158, u'b'));
    QCOMPARE(reviewer_id_boundary.resources.front()
                 .document.value(QStringLiteral("reviewer"))
                 .toObject()
                 .value(QStringLiteral("reviewer_id"))
                 .toString()
                 .toUtf8()
                 .size(),
             160);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &reviewer_id_boundary).has_value());
    reviewer_id_boundary = with_reviewer_field(fixture->detached, QStringLiteral("reviewer_id"),
                                               QStringLiteral("a.") + QString(159, u'b'));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &reviewer_id_boundary).has_value());

    for (const auto& field : {QStringLiteral("reviewer_id"), QStringLiteral("display_name"),
                              QStringLiteral("qualification")}) {
        auto missing = fixture->detached;
        reviewer = missing.resources.front().document.value(QStringLiteral("reviewer")).toObject();
        reviewer.remove(field);
        missing.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &missing).has_value());

        const auto wrong_type = with_reviewer_field(fixture->detached, field, 7);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_type).has_value());
    }

    for (const auto& invalid_reviewer : {QJsonValue{QJsonValue::Undefined}, QJsonValue{7}}) {
        auto candidate = fixture->detached;
        if (invalid_reviewer.isUndefined()) {
            candidate.resources.front().document.remove(QStringLiteral("reviewer"));
        } else {
            candidate.resources.front().document.insert(QStringLiteral("reviewer"),
                                                        invalid_reviewer);
        }
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &candidate).has_value());
    }

    auto extra_reviewer_key = fixture->detached;
    reviewer =
        extra_reviewer_key.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("extra"), QStringLiteral("forbidden"));
    extra_reviewer_key.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &extra_reviewer_key).has_value());

    auto null_affiliation = fixture->detached;
    reviewer =
        null_affiliation.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("affiliation"), QJsonValue::Null);
    null_affiliation.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &null_affiliation).has_value());

    auto no_uncertainty = fixture->detached;
    no_uncertainty.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                     QJsonArray{});
    QVERIFY(validateStrictDetached(*fixture, nullptr, &no_uncertainty).has_value());

    for (const auto& invalid_uncertainties : {QJsonValue{7}, QJsonValue{QJsonObject{}}}) {
        auto candidate = fixture->detached;
        candidate.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                    invalid_uncertainties);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &candidate).has_value());
    }
    auto nonobject_uncertainty = fixture->detached;
    nonobject_uncertainty.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                            QJsonArray{QStringLiteral("legacy")});
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &nonobject_uncertainty).has_value());

    auto invalid_uncertainty_shape = fixture->detached;
    auto invalid_uncertainties = invalid_uncertainty_shape.resources.front()
                                     .document.value(QStringLiteral("known_uncertainty"))
                                     .toArray();
    auto invalid_uncertainty = invalid_uncertainties.first().toObject();
    invalid_uncertainty.insert(QStringLiteral("extra"), true);
    invalid_uncertainties.replace(0, invalid_uncertainty);
    invalid_uncertainty_shape.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                                invalid_uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid_uncertainty_shape).has_value());

    auto invalid_uncertainty_id = fixture->detached;
    invalid_uncertainties = invalid_uncertainty_id.resources.front()
                                .document.value(QStringLiteral("known_uncertainty"))
                                .toArray();
    invalid_uncertainty = invalid_uncertainties.first().toObject();
    invalid_uncertainty.insert(QStringLiteral("uncertainty_id"), QStringLiteral("invalid id"));
    invalid_uncertainties.replace(0, invalid_uncertainty);
    invalid_uncertainty_id.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                             invalid_uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid_uncertainty_id).has_value());

    auto uncertainty_id_boundary = with_uncertainty_field(
        fixture->detached, QStringLiteral("uncertainty_id"), QStringLiteral("a.b"));
    QVERIFY(validateStrictDetached(*fixture, nullptr, &uncertainty_id_boundary).has_value());
    uncertainty_id_boundary =
        with_uncertainty_field(fixture->detached, QStringLiteral("uncertainty_id"),
                               QStringLiteral("a.") + QString(158, u'b'));
    QCOMPARE(uncertainty_id_boundary.resources.front()
                 .document.value(QStringLiteral("known_uncertainty"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("uncertainty_id"))
                 .toString()
                 .toUtf8()
                 .size(),
             160);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &uncertainty_id_boundary).has_value());
    uncertainty_id_boundary =
        with_uncertainty_field(fixture->detached, QStringLiteral("uncertainty_id"),
                               QStringLiteral("a.") + QString(159, u'b'));
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &uncertainty_id_boundary).has_value());

    for (const auto& field : {QStringLiteral("uncertainty_id"), QStringLiteral("summary"),
                              QStringLiteral("blocking")}) {
        auto missing = fixture->detached;
        auto values =
            missing.resources.front().document.value(QStringLiteral("known_uncertainty")).toArray();
        auto item = values.first().toObject();
        item.remove(field);
        values.replace(0, item);
        missing.resources.front().document.insert(QStringLiteral("known_uncertainty"), values);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &missing).has_value());

        const auto wrong_type = with_uncertainty_field(
            fixture->detached, field,
            field == QStringLiteral("blocking") ? QJsonValue{7} : QJsonValue{true});
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_type).has_value());
    }

    auto duplicate_uncertainty = fixture->detached;
    auto duplicate_uncertainties = duplicate_uncertainty.resources.front()
                                       .document.value(QStringLiteral("known_uncertainty"))
                                       .toArray();
    duplicate_uncertainties.push_back(duplicate_uncertainties.first());
    duplicate_uncertainty.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                            duplicate_uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &duplicate_uncertainty).has_value());

    for (const auto& summary : {QString{}, QStringLiteral(" padded"), QStringLiteral("padded "),
                                QStringLiteral(" \t\n")}) {
        auto invalid_summary = fixture->detached;
        auto values = invalid_summary.resources.front()
                          .document.value(QStringLiteral("known_uncertainty"))
                          .toArray();
        auto item = values.first().toObject();
        item.insert(QStringLiteral("summary"), summary);
        values.replace(0, item);
        invalid_summary.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                          values);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid_summary).has_value());
    }

    auto summary_boundary = fixture->detached;
    auto summary_values = summary_boundary.resources.front()
                              .document.value(QStringLiteral("known_uncertainty"))
                              .toArray();
    auto summary_item = summary_values.first().toObject();
    summary_item.insert(QStringLiteral("summary"), multibyte_boundary(2'048, false));
    summary_values.replace(0, summary_item);
    summary_boundary.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                       summary_values);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &summary_boundary).has_value());
    summary_item.insert(QStringLiteral("summary"), multibyte_boundary(2'048, true));
    summary_values.replace(0, summary_item);
    summary_boundary.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                       summary_values);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &summary_boundary).has_value());

    auto blocking = fixture->detached;
    auto uncertainties =
        blocking.resources.front().document.value(QStringLiteral("known_uncertainty")).toArray();
    auto first = uncertainties.first().toObject();
    first.insert(QStringLiteral("blocking"), true);
    first.insert(QStringLiteral("remediation_issue"),
                 QStringLiteral("https://example.invalid/review/%2Fissue"));
    uncertainties.replace(0, first);
    blocking.resources.front().document.insert(QStringLiteral("known_uncertainty"), uncertainties);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &blocking).has_value());

    auto canonical_query = blocking;
    uncertainties = canonical_query.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.insert(QStringLiteral("remediation_issue"),
                 QStringLiteral("https://example.invalid/review?next=%3A%2F"));
    uncertainties.replace(0, first);
    canonical_query.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                      uncertainties);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &canonical_query).has_value());

    const auto remediation_prefix = QStringLiteral("https://example.invalid/");
    QCOMPARE(remediation_prefix.toUtf8().size(), 24);
    auto remediation_boundary = blocking;
    uncertainties = remediation_boundary.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.insert(QStringLiteral("remediation_issue"), remediation_prefix + QString(2'024, u'x'));
    uncertainties.replace(0, first);
    remediation_boundary.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                           uncertainties);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &remediation_boundary).has_value());
    first.insert(QStringLiteral("remediation_issue"), remediation_prefix + QString(2'025, u'x'));
    uncertainties.replace(0, first);
    remediation_boundary.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                           uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &remediation_boundary).has_value());

    auto missing_remediation = blocking;
    uncertainties = missing_remediation.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.remove(QStringLiteral("remediation_issue"));
    uncertainties.replace(0, first);
    missing_remediation.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                          uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &missing_remediation).has_value());

    auto nonblocking_remediation = fixture->detached;
    uncertainties = nonblocking_remediation.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.insert(QStringLiteral("remediation_issue"),
                 QStringLiteral("https://example.invalid/review"));
    uncertainties.replace(0, first);
    nonblocking_remediation.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                              uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &nonblocking_remediation).has_value());

    auto noncanonical_remediation = blocking;
    uncertainties = noncanonical_remediation.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.insert(QStringLiteral("remediation_issue"),
                 QStringLiteral("https://example.invalid/review/%2fissue"));
    uncertainties.replace(0, first);
    noncanonical_remediation.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                               uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &noncanonical_remediation).has_value());

    for (const auto& invalid_url : {
             QStringLiteral("http://example.invalid/review"),
             QStringLiteral("https://user@example.invalid/review"),
             QStringLiteral("https://example.invalid:443/review"),
             QStringLiteral("https://example.invalid/review#fragment"),
             QStringLiteral("https://Example.invalid/review"),
             QStringLiteral("https://example.invalid/review/%GG"),
             QStringLiteral("https://example.invalid/review with-space"),
             QStringLiteral("https://example.invalid/r\u00E9view"),
         }) {
        auto invalid_remediation = blocking;
        auto values = invalid_remediation.resources.front()
                          .document.value(QStringLiteral("known_uncertainty"))
                          .toArray();
        auto item = values.first().toObject();
        item.insert(QStringLiteral("remediation_issue"), invalid_url);
        values.replace(0, item);
        invalid_remediation.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                              values);
        QVERIFY(!validateStrictDetached(*fixture, nullptr, &invalid_remediation).has_value());
    }

    auto wrong_remediation_type = blocking;
    uncertainties = wrong_remediation_type.resources.front()
                        .document.value(QStringLiteral("known_uncertainty"))
                        .toArray();
    first = uncertainties.first().toObject();
    first.insert(QStringLiteral("remediation_issue"), 7);
    uncertainties.replace(0, first);
    wrong_remediation_type.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                             uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &wrong_remediation_type).has_value());

    auto maximum_uncertainty = fixture->detached;
    QJsonArray maximum_uncertainties;
    for (int index = 0; index < 256; ++index) {
        maximum_uncertainties.push_back(QJsonObject{
            {QStringLiteral("uncertainty_id"), QStringLiteral("test.uncertainty.%1").arg(index)},
            {QStringLiteral("summary"), QStringLiteral("TEST-ONLY uncertainty")},
            {QStringLiteral("blocking"), false},
        });
    }
    maximum_uncertainty.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                          maximum_uncertainties);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &maximum_uncertainty).has_value());
    maximum_uncertainties.push_back(QJsonObject{
        {QStringLiteral("uncertainty_id"), QStringLiteral("test.uncertainty.overflow")},
        {QStringLiteral("summary"), QStringLiteral("TEST-ONLY uncertainty")},
        {QStringLiteral("blocking"), false},
    });
    maximum_uncertainty.resources.front().document.insert(QStringLiteral("known_uncertainty"),
                                                          maximum_uncertainties);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &maximum_uncertainty).has_value());
}

void SchemaDispatchTest::enforcesCodeOwnedRealismScorePrerequisites() {
    const auto baseline = strictDetachedFixture();
    QVERIFY(baseline.has_value());
    const auto validate_subject = [](const StrictDetachedFixture& fixture,
                                     const appellate::packs::LoadedPack& subject) {
        const std::array<const appellate::packs::LoadedPack*, 3> dependencies{
            &fixture.federal, &fixture.ca4, &fixture.bench};
        return PackReader::validateResolvedGraph(subject, dependencies);
    };
    const auto expect_targeted_failure = [](const auto& result, const QString& needle) {
        return !result.has_value() && result.error().message.contains(needle);
    };

    QVERIFY(validate_subject(*baseline, baseline->subject).has_value());
    QVERIFY(validateStrictDetached(*baseline).has_value());

    auto ordinary_runtime_subject =
        PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY(ordinary_runtime_subject.has_value());
    std::erase_if(ordinary_runtime_subject->resources, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::ArgumentConfig ||
               resource.descriptor.kind == appellate::model::ResourceKind::RealismReview;
    });
    const auto ordinary_runtime_result =
        appellate::packs::loadRuntimePack(*ordinary_runtime_subject);
    QVERIFY(!ordinary_runtime_result.has_value());
    QCOMPARE(ordinary_runtime_result.error().code,
             appellate::packs::RuntimePackErrorCode::MissingArgumentConfiguration);

    auto single_positive = baseline->subject;
    auto* single_review = firstRealismReview(single_positive);
    QVERIFY(single_review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *single_review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
    refreshResourceDigest(*single_review);
    refreshPackRevision(single_positive);
    QVERIFY(validate_subject(*baseline, single_positive).has_value());

    auto no_prerequisites = *baseline;
    const auto removed = removeSubjectResources(no_prerequisites.subject, [](const auto& resource) {
        return resource.descriptor.kind == appellate::model::ResourceKind::ArgumentConfig;
    });
    QVERIFY(!removed.isEmpty());
    auto* no_prerequisite_source = firstRealismReview(no_prerequisites.subject);
    QVERIFY(no_prerequisite_source != nullptr);
    repairReviewAfterRemovedResources(*no_prerequisite_source, removed);
    repairReviewAfterRemovedResources(no_prerequisites.detached.resources.front(), removed);
    for (const auto& dimension :
         {QStringLiteral("oral_argument"), QStringLiteral("bench_differentiation")}) {
        setReviewDimension(*no_prerequisite_source, dimension, 0);
        setReviewDimension(no_prerequisites.detached.resources.front(), dimension, 0);
    }
    refreshStrictDetachedFixture(no_prerequisites);

    const auto zero_multi_result = validate_subject(no_prerequisites, no_prerequisites.subject);
    QVERIFY2(zero_multi_result.has_value(),
             zero_multi_result ? "" : qPrintable(zero_multi_result.error().message));
    QVERIFY(validateStrictDetached(no_prerequisites).has_value());
    auto single_zero = no_prerequisites.subject;
    single_review = firstRealismReview(single_zero);
    QVERIFY(single_review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *single_review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
    setReviewDimension(*single_review, QStringLiteral("oral_argument"), 0);
    setReviewDimension(*single_review, QStringLiteral("bench_differentiation"), 0);
    refreshResourceDigest(*single_review);
    refreshPackRevision(single_zero);
    QVERIFY(validate_subject(no_prerequisites, single_zero).has_value());

    auto manual_missing_arguments = no_prerequisites.subject;
    auto* manual_review = firstRealismReview(manual_missing_arguments);
    QVERIFY(manual_review != nullptr);
    useManualTraceProfile(*manual_review);
    refreshResourceDigest(*manual_review);
    refreshPackRevision(manual_missing_arguments);
    const auto manual_missing_result = validate_subject(no_prerequisites, manual_missing_arguments);
    QVERIFY(expect_targeted_failure(manual_missing_result,
                                    QStringLiteral("no oral-argument configuration")));

    for (const auto& dimension :
         {QStringLiteral("oral_argument"), QStringLiteral("bench_differentiation")}) {
        auto missing_multi = no_prerequisites.subject;
        auto* review = firstRealismReview(missing_multi);
        QVERIFY(review != nullptr);
        setReviewDimension(*review, dimension, 2, true);
        refreshResourceDigest(*review);
        refreshPackRevision(missing_multi);
        const auto multi_result = validate_subject(no_prerequisites, missing_multi);
        QVERIFY(expect_targeted_failure(multi_result,
                                        dimension == QStringLiteral("oral_argument")
                                            ? QStringLiteral("case-targeted argument configuration")
                                            : QStringLiteral("bench and judge profiles")));

        auto missing_single = missing_multi;
        review = firstRealismReview(missing_single);
        QVERIFY(review != nullptr);
        QVERIFY(setReviewTraceProfile(
            *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
        setReviewDimension(*review, QStringLiteral("oral_argument"), 0);
        setReviewDimension(*review, QStringLiteral("bench_differentiation"), 0);
        setReviewDimension(*review, dimension, 1, true);
        refreshResourceDigest(*review);
        refreshPackRevision(missing_single);
        const auto single_result = validate_subject(no_prerequisites, missing_single);
        QVERIFY(expect_targeted_failure(single_result,
                                        dimension == QStringLiteral("oral_argument")
                                            ? QStringLiteral("case-targeted argument configuration")
                                            : QStringLiteral("bench and judge profiles")));

        auto missing_detached = no_prerequisites;
        setReviewDimension(missing_detached.detached.resources.front(), dimension, 3, true);
        refreshStrictDetachedFixture(missing_detached);
        const auto detached_result = validateStrictDetached(missing_detached);
        QVERIFY(expect_targeted_failure(detached_result,
                                        dimension == QStringLiteral("oral_argument")
                                            ? QStringLiteral("case-targeted argument configuration")
                                            : QStringLiteral("bench and judge profiles")));
    }

    auto missing_bench_resource = no_prerequisites;
    const auto removed_bench =
        removeSubjectResources(missing_bench_resource.subject, [](const auto& resource) {
            return resource.descriptor.kind == appellate::model::ResourceKind::BenchConfiguration;
        });
    QVERIFY(!removed_bench.isEmpty());
    auto* missing_bench_source = firstRealismReview(missing_bench_resource.subject);
    QVERIFY(missing_bench_source != nullptr);
    repairReviewAfterRemovedResources(*missing_bench_source, removed_bench);
    repairReviewAfterRemovedResources(missing_bench_resource.detached.resources.front(),
                                      removed_bench);
    setReviewDimension(missing_bench_resource.detached.resources.front(),
                       QStringLiteral("bench_differentiation"), 3, true);
    refreshStrictDetachedFixture(missing_bench_resource);
    QVERIFY(expect_targeted_failure(validateStrictDetached(missing_bench_resource),
                                    QStringLiteral("bench and judge profiles")));
}

void SchemaDispatchTest::enforcesCodeOwnedRealismUnicodeScalars() {
    const auto fixture = strictDetachedFixture();
    QVERIFY(fixture.has_value());
    const std::array<const appellate::packs::LoadedPack*, 3> subject_dependencies{
        &fixture->federal, &fixture->ca4, &fixture->bench};
    const QString lone_high{QChar{0xD800}};
    const QString lone_low{QChar{0xDC00}};
    QString supplementary;
    supplementary.append(QChar{0xD83D});
    supplementary.append(QChar{0xDE00});

    const auto set_summary = [](appellate::packs::ValidatedResource& review,
                                const QString& summary) {
        auto uncertainties = review.document.value(QStringLiteral("known_uncertainty")).toArray();
        auto first = uncertainties.first().toObject();
        first.insert(QStringLiteral("summary"), summary);
        uncertainties.replace(0, first);
        review.document.insert(QStringLiteral("known_uncertainty"), uncertainties);
    };

    auto multi_high = fixture->subject;
    auto* review = firstRealismReview(multi_high);
    QVERIFY(review != nullptr);
    set_summary(*review, QStringLiteral("TEST-ONLY ") + lone_high);
    QVERIFY(!PackReader::validateResolvedGraph(multi_high, subject_dependencies).has_value());

    auto multi_low = fixture->subject;
    review = firstRealismReview(multi_low);
    QVERIFY(review != nullptr);
    set_summary(*review, QStringLiteral("TEST-ONLY ") + lone_low);
    QVERIFY(!PackReader::validateResolvedGraph(multi_low, subject_dependencies).has_value());

    auto multi_pair = fixture->subject;
    review = firstRealismReview(multi_pair);
    QVERIFY(review != nullptr);
    set_summary(*review, QStringLiteral("TEST-ONLY ") + supplementary);
    QVERIFY(PackReader::validateResolvedGraph(multi_pair, subject_dependencies).has_value());

    auto single_high = fixture->subject;
    review = firstRealismReview(single_high);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
    set_summary(*review, QStringLiteral("TEST-ONLY ") + lone_high);
    QVERIFY(!PackReader::validateResolvedGraph(single_high, subject_dependencies).has_value());

    auto detached_high = fixture->detached;
    auto reviewer =
        detached_high.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("display_name"), QStringLiteral("TEST-ONLY ") + lone_high);
    detached_high.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &detached_high).has_value());

    auto detached_low = fixture->detached;
    reviewer = detached_low.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("display_name"), QStringLiteral("TEST-ONLY ") + lone_low);
    detached_low.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(!validateStrictDetached(*fixture, nullptr, &detached_low).has_value());

    auto detached_pair = fixture->detached;
    reviewer =
        detached_pair.resources.front().document.value(QStringLiteral("reviewer")).toObject();
    reviewer.insert(QStringLiteral("display_name"), QStringLiteral("TEST-ONLY ") + supplementary);
    detached_pair.resources.front().document.insert(QStringLiteral("reviewer"), reviewer);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &detached_pair).has_value());

    const auto require_document_scalar_failure = [](const auto& result) {
        return !result.has_value() && result.error().message.contains(QStringLiteral(
                                          "Unicode-scalar object keys and string values"));
    };
    for (const auto& invalid_scalar : {lone_high, lone_low}) {
        const auto invalid_key = QStringLiteral("test.") + invalid_scalar;

        auto multi_key = fixture->subject;
        review = firstRealismReview(multi_key);
        QVERIFY(review != nullptr);
        review->document.insert(invalid_key, true);
        QVERIFY(require_document_scalar_failure(
            PackReader::validateResolvedGraph(multi_key, subject_dependencies)));

        auto single_key = fixture->subject;
        review = firstRealismReview(single_key);
        QVERIFY(review != nullptr);
        QVERIFY(setReviewTraceProfile(
            *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
        review->document.insert(invalid_key, true);
        QVERIFY(require_document_scalar_failure(
            PackReader::validateResolvedGraph(single_key, subject_dependencies)));

        auto detached_key = fixture->detached;
        detached_key.resources.front().document.insert(invalid_key, true);
        QVERIFY(require_document_scalar_failure(
            validateStrictDetached(*fixture, nullptr, &detached_key)));
    }

    const auto supplementary_key = QStringLiteral("test.") + supplementary;
    auto multi_key_pair = fixture->subject;
    review = firstRealismReview(multi_key_pair);
    QVERIFY(review != nullptr);
    review->document.insert(supplementary_key, true);
    QVERIFY(PackReader::validateResolvedGraph(multi_key_pair, subject_dependencies).has_value());

    auto single_key_pair = fixture->subject;
    review = firstRealismReview(single_key_pair);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
    review->document.insert(supplementary_key, true);
    QVERIFY(PackReader::validateResolvedGraph(single_key_pair, subject_dependencies).has_value());

    auto detached_key_pair = fixture->detached;
    detached_key_pair.resources.front().document.insert(supplementary_key, true);
    QVERIFY(validateStrictDetached(*fixture, nullptr, &detached_key_pair).has_value());

    const auto require_scalar_journal_failure = [](const auto& result) {
        return !result.has_value() &&
               result.error().message.contains(QStringLiteral("canonical journal JSON contains a "
                                                              "non-scalar"));
    };
    for (const auto& invalid_scalar : {lone_high, lone_low}) {
        auto multi_journal = fixture->subject;
        review = firstRealismReview(multi_journal);
        QVERIFY(review != nullptr);
        QVERIFY(
            mutateFirstTraceCommandField(*review, QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(*review);
        refreshPackRevision(multi_journal);
        QVERIFY(require_scalar_journal_failure(
            PackReader::validateResolvedGraph(multi_journal, subject_dependencies)));

        auto single_journal = fixture->subject;
        review = firstRealismReview(single_journal);
        QVERIFY(review != nullptr);
        QVERIFY(setReviewTraceProfile(
            *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
        QVERIFY(
            mutateFirstTraceCommandField(*review, QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(*review);
        refreshPackRevision(single_journal);
        QVERIFY(require_scalar_journal_failure(
            PackReader::validateResolvedGraph(single_journal, subject_dependencies)));

        auto detached_journal = *fixture;
        QVERIFY(mutateFirstTraceCommandField(detached_journal.detached.resources.front(),
                                             QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(detached_journal.detached.resources.front());
        refreshPackRevision(detached_journal.detached);
        QVERIFY(require_scalar_journal_failure(validateStrictDetached(detached_journal)));

        auto multi_event = fixture->subject;
        review = firstRealismReview(multi_event);
        QVERIFY(review != nullptr);
        QVERIFY(mutateFirstTraceEventProposition(*review,
                                                 QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(*review);
        refreshPackRevision(multi_event);
        QVERIFY(require_scalar_journal_failure(
            PackReader::validateResolvedGraph(multi_event, subject_dependencies)));

        auto single_event = fixture->subject;
        review = firstRealismReview(single_event);
        QVERIFY(review != nullptr);
        QVERIFY(setReviewTraceProfile(
            *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
        QVERIFY(mutateFirstTraceEventProposition(*review,
                                                 QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(*review);
        refreshPackRevision(single_event);
        QVERIFY(require_scalar_journal_failure(
            PackReader::validateResolvedGraph(single_event, subject_dependencies)));

        auto detached_event = *fixture;
        QVERIFY(mutateFirstTraceEventProposition(detached_event.detached.resources.front(),
                                                 QStringLiteral("TEST-ONLY ") + invalid_scalar));
        refreshResourceDigest(detached_event.detached.resources.front());
        refreshPackRevision(detached_event.detached);
        QVERIFY(require_scalar_journal_failure(validateStrictDetached(detached_event)));
    }

    auto multi_journal_pair = fixture->subject;
    review = firstRealismReview(multi_journal_pair);
    QVERIFY(review != nullptr);
    QVERIFY(mutateFirstTraceCommandField(*review, QStringLiteral("TEST-ONLY ") + supplementary));
    refreshResourceDigest(*review);
    refreshPackRevision(multi_journal_pair);
    QVERIFY(
        PackReader::validateResolvedGraph(multi_journal_pair, subject_dependencies).has_value());

    auto single_journal_pair = fixture->subject;
    review = firstRealismReview(single_journal_pair);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1"), true));
    QVERIFY(mutateFirstTraceCommandField(*review, QStringLiteral("TEST-ONLY ") + supplementary));
    refreshResourceDigest(*review);
    refreshPackRevision(single_journal_pair);
    QVERIFY(
        PackReader::validateResolvedGraph(single_journal_pair, subject_dependencies).has_value());

    auto detached_journal_pair = *fixture;
    auto* paired_source = firstRealismReview(detached_journal_pair.subject);
    QVERIFY(paired_source != nullptr);
    QVERIFY(
        mutateFirstTraceCommandField(*paired_source, QStringLiteral("TEST-ONLY ") + supplementary));
    replayDetachedTracesFromSource(*paired_source,
                                   detached_journal_pair.detached.resources.front());
    refreshStrictDetachedFixture(detached_journal_pair);
    QVERIFY(validateStrictDetached(detached_journal_pair).has_value());

    auto manual = fixture->subject;
    review = firstRealismReview(manual);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(*review, QStringLiteral("test.manual.realism-profile"), false));
    set_summary(*review, QStringLiteral("TEST-ONLY ") + lone_high);
    QVERIFY(PackReader::validateResolvedGraph(manual, subject_dependencies).has_value());

    auto manual_key = fixture->subject;
    review = firstRealismReview(manual_key);
    QVERIFY(review != nullptr);
    useManualTraceProfile(*review);
    review->document.insert(QStringLiteral("test.") + lone_low, true);
    QVERIFY(PackReader::validateResolvedGraph(manual_key, subject_dependencies).has_value());

    auto near_miss_unknown = fixture->subject;
    review = firstRealismReview(near_miss_unknown);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1-near-miss"), false));
    set_summary(*review, QStringLiteral("TEST-ONLY ") + lone_low);
    review->document.insert(QStringLiteral("test.") + lone_high, true);
    QVERIFY(PackReader::validateResolvedGraph(near_miss_unknown, subject_dependencies).has_value());

    auto manual_journal = fixture->subject;
    review = firstRealismReview(manual_journal);
    QVERIFY(review != nullptr);
    useManualTraceProfile(*review);
    QVERIFY(mutateFirstTraceCommandField(*review, QStringLiteral("TEST-ONLY ") + lone_high));
    const auto manual_journal_result =
        PackReader::validateResolvedGraph(manual_journal, subject_dependencies);
    QVERIFY(!manual_journal_result.has_value());
    QVERIFY(!manual_journal_result.error().message.contains(
        QStringLiteral("canonical journal JSON contains a non-scalar")));

    auto unknown_journal = fixture->subject;
    review = firstRealismReview(unknown_journal);
    QVERIFY(review != nullptr);
    QVERIFY(setReviewTraceProfile(
        *review, QStringLiteral("appellate.realism-evidence.codec-replay.v1-near-miss"), false));
    QVERIFY(mutateFirstTraceEventProposition(*review, QStringLiteral("TEST-ONLY ") + lone_low));
    const auto unknown_journal_result =
        PackReader::validateResolvedGraph(unknown_journal, subject_dependencies);
    QVERIFY(!unknown_journal_result.has_value());
    QVERIFY(!unknown_journal_result.error().message.contains(
        QStringLiteral("canonical journal JSON contains a non-scalar")));
}

void SchemaDispatchTest::rejectsUnsupportedKindVersions() {
    QTemporaryDir unsupported_version;
    QVERIFY(unsupported_version.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unsupported_version.path()));
    QVERIFY(mutateManifest(unsupported_version.path(), [](QJsonObject& manifest) {
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        auto descriptor = contents.at(0).toObject();
        descriptor.insert(QStringLiteral("schema_version"), 3);
        contents.replace(0, descriptor);
        manifest.insert(QStringLiteral("contents"), contents);
    }));
    auto result = PackReader::readDirectory(unsupported_version.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, ErrorCode::UnsupportedSchema);

    QTemporaryDir unsupported_kind;
    QVERIFY(unsupported_kind.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unsupported_kind.path()));
    QVERIFY(mutateManifest(unsupported_kind.path(), [](QJsonObject& manifest) {
        auto contents = manifest.value(QStringLiteral("contents")).toArray();
        auto descriptor = contents.at(0).toObject();
        descriptor.insert(QStringLiteral("kind"), QStringLiteral("native_plugin"));
        contents.replace(0, descriptor);
        manifest.insert(QStringLiteral("contents"), contents);
    }));
    result = PackReader::readDirectory(unsupported_kind.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, ErrorCode::UnsupportedResourceKind);
}

void SchemaDispatchTest::rejectsV1V2CrossInterpretation() {
    QTemporaryDir descriptor_mix;
    QVERIFY(descriptor_mix.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), descriptor_mix.path()));
    QVERIFY(mutateManifest(descriptor_mix.path(), [](QJsonObject& manifest) {
        manifest.insert(QStringLiteral("schema_version"), 1);
        manifest.insert(
            QStringLiteral("required_capabilities"),
            QJsonArray{QJsonObject{
                {QStringLiteral("id"), QStringLiteral("workbench.pack.declarative-resources")},
                {QStringLiteral("version"), 1},
            }});
    }));
    auto result = PackReader::readDirectory(descriptor_mix.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, ErrorCode::UnsupportedSchema);

    QTemporaryDir payload_mix;
    QVERIFY(payload_mix.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), payload_mix.path()));
    QVERIFY(setResourceSchemaVersion(payload_mix.path(), QStringLiteral("resources/case.json"), 1));
    result = PackReader::readDirectory(payload_mix.path());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, ErrorCode::SchemaViolation);
}

void SchemaDispatchTest::validatesSharedWorkflowCapabilitySchemas() {
    const auto schema_rejects = [&](const QString& fixture_name, auto mutation) {
        QTemporaryDir pack;
        if (!pack.isValid() || !copyTree(fixture(fixture_name), pack.path()) ||
            !mutateResource(pack.path(), QStringLiteral("resources/workflow.json"), mutation)) {
            return false;
        }
        const auto rejected = PackReader::readDirectory(pack.path());
        return !rejected.has_value() && rejected.error().code == ErrorCode::SchemaViolation;
    };
    const auto mutate_operation = [](QJsonObject& workflow, const QString& operation_id,
                                     auto mutation) {
        auto operations = workflow.value(QStringLiteral("operations")).toArray();
        for (qsizetype index = 0; index < operations.size(); ++index) {
            auto operation = operations.at(index).toObject();
            if (operation.value(QStringLiteral("operation_id")).toString() != operation_id) {
                continue;
            }
            mutation(operation);
            operations.replace(index, operation);
            workflow.insert(QStringLiteral("operations"), operations);
            return true;
        }
        return false;
    };

    for (int variant = 0; variant < 7; ++variant) {
        QVERIFY(schema_rejects(QStringLiteral("full-resource-pack-v2"), [&](QJsonObject& workflow) {
            return mutate_operation(
                workflow, QStringLiteral("example.operation.reject-opened"),
                [&](QJsonObject& operation) {
                    QJsonObject time{
                        {QStringLiteral("court_date"), QStringLiteral("2026-01-02")},
                        {QStringLiteral("instant_unix_seconds"), QStringLiteral("0")},
                    };
                    QJsonArray times{time};
                    if (variant == 0) {
                        times = {};
                    } else if (variant == 1) {
                        times.push_back(time);
                    } else if (variant == 2) {
                        time.insert(QStringLiteral("unexpected"), true);
                        times = {time};
                    } else if (variant == 3) {
                        time.remove(QStringLiteral("court_date"));
                        times = {time};
                    } else if (variant == 4) {
                        time.insert(QStringLiteral("instant_unix_seconds"), 0);
                        times = {time};
                    } else if (variant == 5) {
                        time.insert(QStringLiteral("instant_unix_seconds"), QStringLiteral("+1"));
                        times = {time};
                    } else {
                        time.insert(QStringLiteral("instant_unix_seconds"),
                                    QStringLiteral("-10000000000000000000"));
                        times = {time};
                    }
                    operation.insert(QStringLiteral("allowed_legal_times"), times);
                });
        }));
    }

    for (int variant = 0; variant < 6; ++variant) {
        QVERIFY(schema_rejects(QStringLiteral("full-resource-pack-v2"), [&](QJsonObject& workflow) {
            return mutate_operation(
                workflow, QStringLiteral("example.operation.calculate-cure"),
                [&](QJsonObject& operation) {
                    QJsonObject base{
                        {QStringLiteral("kind"), QStringLiteral("order_occurred_one_of")},
                        {QStringLiteral("order_id"), QStringLiteral("example.order.bound")},
                        {QStringLiteral("operation_ids"),
                         QJsonArray{QStringLiteral("example.operation.enter-order")}},
                    };
                    if (variant == 0) {
                        base.insert(QStringLiteral("operation_ids"), QJsonArray{});
                    } else if (variant == 1) {
                        base.insert(QStringLiteral("operation_ids"),
                                    QJsonArray{QStringLiteral("example.operation.enter-order"),
                                               QStringLiteral("example.operation.enter-order")});
                    } else if (variant == 2) {
                        base.insert(QStringLiteral("unexpected"), true);
                    } else if (variant == 3) {
                        base.remove(QStringLiteral("order_id"));
                    } else if (variant == 4) {
                        base.insert(QStringLiteral("kind"), QStringLiteral("order_occurred_any"));
                    } else {
                        base.remove(QStringLiteral("operation_ids"));
                    }
                    operation.insert(QStringLiteral("deadline_event_base"), base);
                });
        }));
    }

    QVERIFY(schema_rejects(QStringLiteral("full-resource-pack-v2"), [](QJsonObject& workflow) {
        auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
        auto route = routes.first().toObject();
        route.insert(
            QStringLiteral("filing_bindings"),
            QJsonArray{QJsonObject{
                {QStringLiteral("filing_id"), QStringLiteral("example.filing.bound")},
                {QStringLiteral("actor_id"), QStringLiteral("example.actor.appellant")},
                {QStringLiteral("record_entry_id"), QStringLiteral("example.record.entry-one")},
                {QStringLiteral("document_sha256"),
                 QStringLiteral(
                     "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366")},
                {QStringLiteral("expected_legal_time"),
                 QJsonObject{
                     {QStringLiteral("court_date"), QStringLiteral("2026-01-02")},
                     {QStringLiteral("instant_unix_seconds"), QStringLiteral("0")},
                 }},
            }});
        routes.replace(0, route);
        workflow.insert(QStringLiteral("filing_routes"), routes);
    }));

    for (int variant = 0; variant < 3; ++variant) {
        QVERIFY(schema_rejects(QStringLiteral("full-resource-pack"), [&](QJsonObject& workflow) {
            if (variant < 2) {
                auto operations = workflow.value(QStringLiteral("operations")).toArray();
                auto operation = operations.first().toObject();
                if (variant == 0) {
                    operation.insert(
                        QStringLiteral("allowed_legal_times"),
                        QJsonArray{QJsonObject{
                            {QStringLiteral("court_date"), QStringLiteral("2026-01-02")},
                            {QStringLiteral("instant_unix_seconds"), QStringLiteral("0")},
                        }});
                } else {
                    operation.insert(
                        QStringLiteral("deadline_event_base"),
                        QJsonObject{
                            {QStringLiteral("kind"), QStringLiteral("order_occurred_one_of")},
                            {QStringLiteral("order_id"), QStringLiteral("example.order.bound")},
                            {QStringLiteral("operation_ids"),
                             QJsonArray{QStringLiteral("example.operation.enter-order")}},
                        });
                }
                operations.replace(0, operation);
                workflow.insert(QStringLiteral("operations"), operations);
            } else {
                auto routes = workflow.value(QStringLiteral("filing_routes")).toArray();
                auto route = routes.first().toObject();
                route.insert(QStringLiteral("filing_bindings"), QJsonArray{});
                routes.replace(0, route);
                workflow.insert(QStringLiteral("filing_routes"), routes);
            }
        }));
    }

    for (const auto& capability_id :
         {QStringLiteral("workbench.pack.route-filing-bindings"),
          QStringLiteral("workbench.pack.alternative-event-date-deadlines"),
          QStringLiteral("workbench.pack.operation-legal-time-guards")}) {
        QTemporaryDir legacy;
        QVERIFY(legacy.isValid());
        QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack")), legacy.path()));
        QVERIFY(mutateManifest(legacy.path(), [&](QJsonObject& manifest) {
            auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
            capabilities.push_back(
                QJsonObject{{QStringLiteral("id"), capability_id}, {QStringLiteral("version"), 1}});
            manifest.insert(QStringLiteral("required_capabilities"), capabilities);
        }));
        const auto rejected = PackReader::readDirectory(legacy.path());
        QVERIFY(!rejected.has_value());
        QCOMPARE(rejected.error().code, ErrorCode::UnsupportedCapability);
    }

    QTemporaryDir divergent_union;
    QVERIFY(divergent_union.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), divergent_union.path()));
    QVERIFY(mutateManifest(divergent_union.path(), [](QJsonObject& manifest) {
        auto capabilities = manifest.value(QStringLiteral("required_capabilities")).toArray();
        for (const auto& id : {QStringLiteral("workbench.pack.named-deadlines"),
                               QStringLiteral("workbench.pack.event-date-deadlines"),
                               QStringLiteral("workbench.pack.alternative-event-date-deadlines"),
                               QStringLiteral("workbench.pack.operation-document-bindings")}) {
            capabilities.push_back(
                QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("version"), 1}});
        }
        manifest.insert(QStringLiteral("required_capabilities"), capabilities);
    }));
    QVERIFY(mutateResource(
        divergent_union.path(), QStringLiteral("resources/workflow.json"),
        [](QJsonObject& workflow) {
            auto operations = workflow.value(QStringLiteral("operations")).toArray();
            QJsonObject court_template;
            QJsonObject deadline_template;
            for (const auto& value : operations) {
                const auto operation = value.toObject();
                const auto id = operation.value(QStringLiteral("operation_id")).toString();
                if (id == QStringLiteral("example.operation.issue-judgment")) {
                    court_template = operation;
                } else if (id == QStringLiteral("example.operation.calculate-cure")) {
                    deadline_template = operation;
                }
            }
            if (court_template.isEmpty() || deadline_template.isEmpty()) {
                return false;
            }
            constexpr auto digest =
                "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366";
            const auto make_order = [&](const QString& operation_id, const QString& record_id,
                                        const QString& date) {
                auto order = court_template;
                order.insert(QStringLiteral("operation_id"), operation_id);
                order.insert(QStringLiteral("stage_id"), QStringLiteral("example.stage.opened"));
                order.insert(QStringLiteral("opcode"), QStringLiteral("enter_order"));
                order.remove(QStringLiteral("preconditions"));
                order.insert(
                    QStringLiteral("document_binding"),
                    QJsonObject{
                        {QStringLiteral("record_entry_id"), record_id},
                        {QStringLiteral("document_sha256"), QString::fromLatin1(digest)},
                        {QStringLiteral("expected_court_date"), date},
                        {QStringLiteral("order_id"), QStringLiteral("example.order.union")},
                        {QStringLiteral("disposition"), QStringLiteral("granted")},
                    });
                return order;
            };
            const auto first_source = QStringLiteral("example.operation.enter-order-union-first");
            const auto second_source = QStringLiteral("example.operation.enter-order-union-second");
            operations.push_back(make_order(first_source,
                                            QStringLiteral("example.record.entry-one"),
                                            QStringLiteral("2026-01-02")));
            operations.push_back(make_order(second_source,
                                            QStringLiteral("example.record.brief-opening"),
                                            QStringLiteral("2026-01-03")));
            const auto make_clock = [&](const QString& operation_id, const QString& deadline_id,
                                        const QString& source_id) {
                auto clock = deadline_template;
                clock.insert(QStringLiteral("operation_id"), operation_id);
                clock.insert(QStringLiteral("produced_deadline_id"), deadline_id);
                clock.insert(QStringLiteral("authorized_role_ids"),
                             QJsonArray{QStringLiteral("example.role.court")});
                clock.insert(
                    QStringLiteral("deadline_event_base"),
                    QJsonObject{
                        {QStringLiteral("kind"), QStringLiteral("order_occurred_one_of")},
                        {QStringLiteral("order_id"), QStringLiteral("example.order.union")},
                        {QStringLiteral("operation_ids"), QJsonArray{source_id}},
                    });
                return clock;
            };
            operations.push_back(
                make_clock(QStringLiteral("example.operation.calculate-union-first"),
                           QStringLiteral("example.deadline.union-first"), first_source));
            operations.push_back(
                make_clock(QStringLiteral("example.operation.calculate-union-second"),
                           QStringLiteral("example.deadline.union-second"), second_source));
            workflow.insert(QStringLiteral("operations"), operations);
            return true;
        }));
    const auto union_loaded = PackReader::readDirectory(divergent_union.path());
    QVERIFY2(union_loaded.has_value(),
             union_loaded ? "" : qPrintable(union_loaded.error().message));
    const auto union_runtime = appellate::packs::loadRuntimePack(*union_loaded);
    QVERIFY2(union_runtime.has_value(),
             union_runtime ? ""
                           : qPrintable(QString::fromStdString(union_runtime.error().message)));
    const auto& runtime_case = union_runtime->cases.front();
    const auto date = [](int day) {
        return appellate::model::LegalDate{std::chrono::year{2026} / std::chrono::month{1} /
                                           std::chrono::day{static_cast<unsigned>(day)}};
    };
    const auto at = [&](int day) {
        const auto court_date = date(day);
        return appellate::model::LegalTime{
            std::chrono::sys_seconds{std::chrono::sys_days{court_date.value}}, court_date};
    };
    const appellate::model::WorkflowState initial_state{"example.session.union",
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
                                                        std::nullopt,
                                                        std::nullopt};
    const appellate::model::WorkflowCommand enter_first = appellate::model::EnterWorkflowOrder{
        appellate::model::WorkflowCommandHeader{
            initial_state.session_id,
            appellate::model::WorkflowCommandId{"example.command.enter-union-first"},
            appellate::model::ActorId{"example.actor.court"}, at(2)},
        appellate::model::WorkflowOperationId{"example.operation.enter-order-union-first"},
        appellate::model::WorkflowOrderId{"example.order.union"},
        appellate::model::WorkflowOrderDisposition::Granted,
        "bab85fe6529e9832b26196e8f08448b02bbe79e5ae4d4d37d104b278e11f1366",
        std::nullopt};
    const auto order_events = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, enter_first);
    QVERIFY(order_events.has_value());
    const std::vector journal{appellate::model::WorkflowJournalEntry{enter_first, *order_events}};
    const auto after_order = appellate::engine::replayWorkflow(
        runtime_case.workflow, runtime_case.definition, initial_state, journal);
    QVERIFY(after_order.has_value());

    const auto calculate = [&](const std::string& command_id, const std::string& operation_id,
                               const std::string& deadline_id) {
        return appellate::model::WorkflowCommand{appellate::model::CalculateWorkflowDeadline{
            appellate::model::WorkflowCommandHeader{
                initial_state.session_id, appellate::model::WorkflowCommandId{command_id},
                appellate::model::ActorId{"example.actor.court"}, at(3)},
            appellate::model::WorkflowOperationId{operation_id},
            appellate::model::WorkflowDeadlineId{deadline_id}}};
    };
    const auto nonmatching_clock = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, *after_order,
        calculate("example.command.calculate-union-second",
                  "example.operation.calculate-union-second", "example.deadline.union-second"));
    QVERIFY(!nonmatching_clock.has_value());
    QCOMPARE(nonmatching_clock.error().code,
             appellate::engine::WorkflowErrorCode::UnmetPrecondition);
    const auto matching_clock = appellate::engine::decideWorkflow(
        runtime_case.workflow, runtime_case.definition, *after_order,
        calculate("example.command.calculate-union-first",
                  "example.operation.calculate-union-first", "example.deadline.union-first"));
    QVERIFY(matching_clock.has_value());
}

} // namespace

QTEST_GUILESS_MAIN(SchemaDispatchTest)

#include "tst_schema_dispatch.moc"
