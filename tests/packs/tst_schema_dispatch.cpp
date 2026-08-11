#include "appellate/packs/pack_reader.hpp"
#include "appellate/packs/runtime_pack.hpp"

#include <QByteArrayView>
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

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace {

using appellate::packs::ErrorCode;
using appellate::packs::PackReader;

class SchemaDispatchTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesPinnedV1Digests();
    void preservesPinnedPre28V2Revision();
    void loadsV2AndProjectsRuntime();
    void rejectsUnknownAndMismatchedCapabilities();
    void rejectsUnderdeclaredCapabilities();
    void preservesLegacyV2WithoutOptionalFeatures();
    void rejectsUnderdeclaredDispositionAndPreconditionCapabilities();
    void validatesClosedWorkflowPreconditions();
    void validatesStructuredDispositionPlans();
    void enforcesStructuredFeatureBounds();
    void closureAndRuntimeRejectForgedCapabilityCoverage();
    void rejectsInvalidCanonicalAuthorityMetadata();
    void rejectsUnresolvedAndDuplicateAuthoritySelections();
    void resolvesCanonicalAuthorityAcrossDependencyGraph();
    void rejectsUnsupportedKindVersions();
    void rejectsV1V2CrossInterpretation();
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

void SchemaDispatchTest::preservesPinnedPre28V2Revision() {
    QTemporaryDir pack;
    QVERIFY(pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), pack.path()));
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2-pre-28-overlay")),
                     pack.path()));

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
}

void SchemaDispatchTest::loadsV2AndProjectsRuntime() {
    const auto v1 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    const auto v2 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(v1.has_value(), v1 ? "" : qPrintable(v1.error().message));
    QVERIFY2(v2.has_value(), v2 ? "" : qPrintable(v2.error().message));
    QCOMPARE(v2->manifest_schema_version, std::uint32_t{2});
    QCOMPARE(v2->revision.digest,
             std::string("bb5e15c14407788a7d9e5370efa610cd12e84a09ca598781bc2f37210f1d4f8d"));
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
                QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("deadline_status")},
                    {QStringLiteral("deadline_id"), QStringLiteral("example.deadline.pending")},
                    {QStringLiteral("status"), QStringLiteral("not_elapsed")}},
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
    QCOMPARE(judgment->preconditions.size(), std::size_t{6});
    const auto* open =
        std::get_if<appellate::model::WorkflowDeadlinePrecondition>(&judgment->preconditions.at(0));
    const auto* elapsed =
        std::get_if<appellate::model::WorkflowDeadlinePrecondition>(&judgment->preconditions.at(1));
    const auto* order =
        std::get_if<appellate::model::WorkflowOrderPrecondition>(&judgment->preconditions.at(2));
    const auto* not_elapsed =
        std::get_if<appellate::model::WorkflowDeadlinePrecondition>(&judgment->preconditions.at(3));
    const auto* argument =
        std::get_if<appellate::model::WorkflowArgumentPrecondition>(&judgment->preconditions.at(4));
    const auto* issued =
        std::get_if<appellate::model::WorkflowJudgmentPrecondition>(&judgment->preconditions.at(5));
    QVERIFY(open != nullptr && elapsed != nullptr && order != nullptr && not_elapsed != nullptr &&
            argument != nullptr && issued != nullptr);
    QCOMPARE(open->condition, appellate::model::WorkflowDeadlineCondition::Open);
    QCOMPARE(elapsed->condition, appellate::model::WorkflowDeadlineCondition::Elapsed);
    QCOMPARE(order->disposition, appellate::model::WorkflowOrderDisposition::Granted);
    QCOMPARE(not_elapsed->condition, appellate::model::WorkflowDeadlineCondition::NotElapsed);
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
    QVERIFY(appellate::model::isCanonicalAuthorityText(unicode_text.toUtf8().toStdString(), 4096));
    QTemporaryDir unicode_pack;
    QVERIFY(unicode_pack.isValid());
    QVERIFY(copyTree(fixture(QStringLiteral("full-resource-pack-v2")), unicode_pack.path()));
    QVERIFY(mutateResource(unicode_pack.path(), authority_path, [&](QJsonObject& document) {
        auto values = document.value(QStringLiteral("authorities")).toArray();
        auto first = values.at(0).toObject();
        first.insert(QStringLiteral("citation"), unicode_text);
        first.insert(QStringLiteral("proposition"), unicode_text);
        first.insert(QStringLiteral("locator"), unicode_text);
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
             unicode_text.toUtf8().toStdString());

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

} // namespace

QTEST_GUILESS_MAIN(SchemaDispatchTest)

#include "tst_schema_dispatch.moc"
