#include "appellate/packs/pack_reader.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

class PackReaderTest final : public QObject {
    Q_OBJECT

  private slots:
    void loadsValidPack();
    void rejectsMalformedJson();
    void rejectsUnsupportedSchema();
    void rejectsPathTraversal();
    void rejectsDuplicateContentId();
    void rejectsUnknownManifestField();
    void rejectsProhibitedJudgeField();
    void rejectsInvalidIdentifiersVersionsAndHashes();
    void rejectsMalformedCapabilityAndDependency();
    void rejectsDuplicateContentPath();
    void rejectsDuplicatePayloadId();
    void rejectsUndeclaredFile();
    void rejectsIntermediateSymlink();
    void rejectsOversizedJson();
    void producesCanonicalOrderIndependentDigest();
    void digestIncludesPathAndDependencies();
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

[[nodiscard]] QJsonObject validJudge(const QString& id = QStringLiteral("example.judge.measured"),
                                     const QString& name = QStringLiteral("Measured Panelist")) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("profile_id"), id},
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
             {QStringLiteral("verbosity"), 0.46},
             {QStringLiteral("sentence_complexity"), 0.58},
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
                                        const QString& version = QStringLiteral("1.0.0")) {
    return QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("pack_id"), QStringLiteral("example.test.pack")},
        {QStringLiteral("version"), version},
        {QStringLiteral("required_capabilities"), capabilities},
        {QStringLiteral("dependencies"), dependencies},
        {QStringLiteral("contents"), contents},
    };
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
    QCOMPARE(result->judge_profiles.size(), std::size_t{1});
    const auto& profile = result->judge_profiles.front();
    QCOMPARE(profile.display_name, std::string("Measured Panelist"));
    QCOMPARE(profile.profile_class, appellate::model::ProfileClass::FictionalComposite);
    QCOMPARE(profile.compatibility.court_roles.size(), std::size_t{1});
    QCOMPARE(profile.compatibility.jurisdiction_ids.front(), std::string("us.ca4"));
    QCOMPARE(profile.interaction.issue_focus.size(), std::size_t{2});
    QCOMPARE(profile.voice.cadence, appellate::model::VoiceCadence::Measured);
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

} // namespace

QTEST_GUILESS_MAIN(PackReaderTest)

#include "tst_pack_reader.moc"
