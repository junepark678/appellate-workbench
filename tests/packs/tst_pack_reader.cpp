#include "appellate/packs/pack_reader.hpp"

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
    void producesDeterministicDigest();
};

[[nodiscard]] QString fixture(const QString& name) {
    return QStringLiteral(APPELLATE_TEST_FIXTURES) + u'/' + name;
}

void PackReaderTest::loadsValidPack() {
    const auto result =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("minimal-pack")));

    if (!result.has_value()) {
        QFAIL(qPrintable(result.error().message));
    }
    QCOMPARE(result->revision.id.value, std::string("example.appellate.ca4"));
    QCOMPARE(result->revision.version, std::string("0.1.0"));
    QCOMPARE(result->judge_profiles.size(), std::size_t{1});
    QCOMPARE(result->judge_profiles.front().display_name, std::string("Measured Panelist"));
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

void PackReaderTest::producesDeterministicDigest() {
    const auto first =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("minimal-pack")));
    const auto second =
        appellate::packs::PackReader::readDirectory(fixture(QStringLiteral("minimal-pack")));

    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->revision.digest, second->revision.digest);
    QCOMPARE(first->revision.digest.size(), std::size_t{64});
}

} // namespace

QTEST_GUILESS_MAIN(PackReaderTest)

#include "tst_pack_reader.moc"
