#include "appellate/packs/pack_reader.hpp"
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

#include <algorithm>
#include <array>
#include <functional>
#include <string>

namespace {

using appellate::packs::ErrorCode;
using appellate::packs::PackReader;

class SchemaDispatchTest final : public QObject {
    Q_OBJECT

  private slots:
    void preservesPinnedV1Digests();
    void loadsV2AndProjectsRuntime();
    void rejectsUnknownAndMismatchedCapabilities();
    void rejectsUnderdeclaredCapabilities();
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

void SchemaDispatchTest::loadsV2AndProjectsRuntime() {
    const auto v1 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack")));
    const auto v2 = PackReader::readDirectory(fixture(QStringLiteral("full-resource-pack-v2")));
    QVERIFY2(v1.has_value(), v1 ? "" : qPrintable(v1.error().message));
    QVERIFY2(v2.has_value(), v2 ? "" : qPrintable(v2.error().message));
    QCOMPARE(v2->manifest_schema_version, std::uint32_t{2});
    QCOMPARE(v2->revision.digest,
             std::string("e36b712c5f845148a61b65992077119551c0521e39679b0f1572f76217882b54"));
    QVERIFY(v2->revision.digest != v1->revision.digest);
    for (const auto& resource : v2->resources) {
        QCOMPARE(resource.descriptor.schema_version, std::uint32_t{2});
    }
    const auto runtime = appellate::packs::loadRuntimePack(*v2);
    QVERIFY2(runtime.has_value(), runtime ? "" : runtime.error().message.c_str());
    QCOMPARE(runtime->cases.size(), std::size_t{1});
    QCOMPARE(runtime->cases.front().definition.id.value, std::string("example.case.fictional"));
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
