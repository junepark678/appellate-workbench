#include "appellate/content/markdown_pdf_renderer.hpp"
#include "appellate/content/render_cli.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPdfDocument>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using appellate::content::MarkdownPdfRenderer;
using appellate::content::RenderBatchLimits;
using appellate::content::RenderCliExitCode;
using appellate::content::RenderCliResult;
using appellate::content::runRenderCli;

namespace {

constexpr auto marker = "<!-- PAGE BREAK -->";

[[nodiscard]] bool writeBytes(const QString& path, QByteArrayView bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes.data(), bytes.size()) == bytes.size() && file.flush();
}

[[nodiscard]] bool writeJson(const QString& path, const QJsonObject& object) {
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] QJsonObject decodeObject(QByteArrayView bytes) {
    const auto document = QJsonDocument::fromJson(bytes.toByteArray());
    return document.isObject() ? document.object() : QJsonObject{};
}

[[nodiscard]] QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] QJsonObject singleEntry(QString source, QString output, QString title) {
    return QJsonObject{
        {QStringLiteral("output_path"), std::move(output)},
        {QStringLiteral("source_path"), std::move(source)},
        {QStringLiteral("title"), std::move(title)},
    };
}

[[nodiscard]] QJsonObject planWith(QJsonArray entries) {
    return QJsonObject{
        {QStringLiteral("entries"), std::move(entries)},
        {QStringLiteral("schema_version"), 1},
    };
}

[[nodiscard]] QJsonObject inventoryAt(const QString& output_directory) {
    QFile inventory(QDir(output_directory).filePath(QStringLiteral("inventory.json")));
    if (!inventory.open(QIODevice::ReadOnly)) {
        return {};
    }
    return decodeObject(inventory.readAll());
}

[[nodiscard]] QJsonObject entryAt(const QJsonObject& inventory, qsizetype index) {
    const auto entries = inventory.value(QStringLiteral("entries")).toArray();
    return index >= 0 && index < entries.size() ? entries.at(index).toObject() : QJsonObject{};
}

[[nodiscard]] QString errorCode(const RenderCliResult& result) {
    return decodeObject(result.standard_error).value(QStringLiteral("code")).toString();
}

[[nodiscard]] QStringList invocation(const QString& plan, const QString& sources,
                                     const QString& output) {
    return QStringList{plan, sources, output};
}

struct Fixture final {
    QTemporaryDir root;
    QString source_root;
    QString plan_path;

    Fixture()
        : source_root(root.filePath(QStringLiteral("sources"))),
          plan_path(root.filePath(QStringLiteral("plan.json"))) {
        if (root.isValid()) {
            static_cast<void>(QDir{}.mkpath(source_root));
        }
    }
};

} // namespace

class RenderCliTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersSearchableSingleAndCompositePdfsWithInventory();
    void repeatsSemanticIdentityWithoutPromisingPdfBytes();
    void rollsBackTheWholeBatchWhenOneRenderFails();
    void rejectsOverwriteTraversalSymlinksDuplicatesAndUnknownFields();
    void rejectsInvalidCompositeRangesAndExactOneOfViolations();
    void validatesPageLabelConfigurationAndOverflow();
    void enforcesEntryStringSourceInlineAndOutputLimits();
};

void RenderCliTest::rendersSearchableSingleAndCompositePdfsWithInventory() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    const auto alpha_path = QDir(fixture.source_root).filePath(QStringLiteral("alpha.md"));
    const auto record_path = QDir(fixture.source_root).filePath(QStringLiteral("record.md"));
    const QByteArray alpha = QByteArrayLiteral("# Alpha\n\nSearchable alpha first.\n\n") +
                             QByteArray(marker) +
                             QByteArrayLiteral("\n\n# Omega\n\nExcluded omega second.\n");
    const QByteArray record =
        QByteArrayLiteral("# Red\n\nExcluded red page.\n\n") + QByteArray(marker) +
        QByteArrayLiteral("\n\n# Green\n\nSearchable green page.\n\n") + QByteArray(marker) +
        QByteArrayLiteral("\n\n# Blue\n\nSearchable blue page.\n");
    QVERIFY(writeBytes(alpha_path, alpha));
    QVERIFY(writeBytes(record_path, record));

    const QJsonObject composite{
        {QStringLiteral("front_matter_markdown"),
         QStringLiteral("# Cover\n\nSearchable cover page.\n\n%1\n\n# Index\n\n"
                        "Searchable index page.")
             .arg(QString::fromLatin1(marker))},
        {QStringLiteral("output_path"), QStringLiteral("pdfs/z-appendix.pdf")},
        {QStringLiteral("page_label_prefix"), QStringLiteral("JA")},
        {QStringLiteral("page_label_start"), 10},
        {QStringLiteral("segments"),
         QJsonArray{
             QJsonObject{
                 {QStringLiteral("first_page"), 2},
                 {QStringLiteral("last_page"), 3},
                 {QStringLiteral("source_path"), QStringLiteral("record.md")},
             },
             QJsonObject{
                 {QStringLiteral("first_page"), 1},
                 {QStringLiteral("last_page"), 1},
                 {QStringLiteral("source_path"), QStringLiteral("alpha.md")},
             },
         }},
        {QStringLiteral("title"), QStringLiteral("Synthetic Joint Appendix")},
    };
    const auto plan = planWith(QJsonArray{
        composite,
        singleEntry(QStringLiteral("alpha.md"), QStringLiteral("pdfs/a-alpha.pdf"),
                    QStringLiteral("Synthetic Alpha Record")),
    });
    QVERIFY(writeJson(fixture.plan_path, plan));
    const auto output = fixture.root.filePath(QStringLiteral("published"));

    const auto result = runRenderCli(invocation(fixture.plan_path, fixture.source_root, output));
    if (result.exit_code != static_cast<int>(RenderCliExitCode::Success)) {
        QFAIL(result.standard_error.constData());
    }
    QVERIFY(result.standard_error.isEmpty());
    QCOMPARE(decodeObject(result.standard_output).value(QStringLiteral("status")).toString(),
             QStringLiteral("ok"));

    QFile raw_inventory(QDir(output).filePath(QStringLiteral("inventory.json")));
    QVERIFY(raw_inventory.open(QIODevice::ReadOnly));
    const auto inventory_bytes = raw_inventory.readAll();
    QVERIFY(!inventory_bytes.contains('\n'));
    const auto inventory = decodeObject(inventory_bytes);
    QCOMPARE(inventory.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(inventory.value(QStringLiteral("renderer_contract")).toString(),
             MarkdownPdfRenderer::rendererContractVersion().toString());
    QCOMPARE(inventory.value(QStringLiteral("pdf_byte_deterministic")).toBool(), false);
    QCOMPARE(inventory.value(QStringLiteral("entries")).toArray().size(), 2);

    const auto single = entryAt(inventory, 0);
    const auto assembled = entryAt(inventory, 1);
    QCOMPARE(single.value(QStringLiteral("output_path")).toString(),
             QStringLiteral("pdfs/a-alpha.pdf"));
    QCOMPARE(assembled.value(QStringLiteral("output_path")).toString(),
             QStringLiteral("pdfs/z-appendix.pdf"));
    QCOMPARE(
        single.value(QStringLiteral("source_sha256")).toString(),
        QString::fromLatin1(QCryptographicHash::hash(alpha, QCryptographicHash::Sha256).toHex()));
    QCOMPARE(single.value(QStringLiteral("pdf_sha256")).toString(),
             fileSha256(QDir(output).filePath(QStringLiteral("pdfs/a-alpha.pdf"))));
    QCOMPARE(assembled.value(QStringLiteral("pdf_sha256")).toString(),
             fileSha256(QDir(output).filePath(QStringLiteral("pdfs/z-appendix.pdf"))));
    QCOMPARE(single.value(QStringLiteral("byte_size")).toInteger(),
             QFileInfo(QDir(output).filePath(QStringLiteral("pdfs/a-alpha.pdf"))).size());
    QCOMPARE(assembled.value(QStringLiteral("page_count")).toInt(), 5);
    QCOMPARE(assembled.value(QStringLiteral("source_sha256")).toString().size(), 64);
    QCOMPARE(assembled.value(QStringLiteral("semantic_render_sha256")).toString().size(), 64);
    QCOMPARE(assembled.value(QStringLiteral("semantic_plan_sha256")).toString().size(), 64);
    QVERIFY(!single.contains(QStringLiteral("page_labels")));
    const auto page_labels = assembled.value(QStringLiteral("page_labels")).toObject();
    QCOMPARE(page_labels.value(QStringLiteral("prefix")).toString(), QStringLiteral("JA"));
    QCOMPARE(page_labels.value(QStringLiteral("first_number")).toInt(), 10);
    QCOMPARE(page_labels.value(QStringLiteral("last_number")).toInt(), 14);
    QVERIFY(assembled.value(QStringLiteral("renderer_provenance"))
                .toString()
                .contains(QStringLiteral("pdf_byte_determinism=not-guaranteed")));
    QVERIFY(assembled.value(QStringLiteral("renderer_provenance"))
                .toString()
                .contains(QStringLiteral("page_label_prefix=JA\n")));

    const auto assembly = assembled.value(QStringLiteral("assembly_provenance")).toObject();
    QCOMPARE(assembly.value(QStringLiteral("kind")).toString(), QStringLiteral("composite"));
    QCOMPARE(assembly.value(QStringLiteral("logical_page_count")).toInt(), 5);
    QCOMPARE(assembly.value(QStringLiteral("front_matter"))
                 .toObject()
                 .value(QStringLiteral("logical_page_count"))
                 .toInt(),
             2);
    const auto segments = assembly.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 2);
    QCOMPARE(segments.at(0).toObject().value(QStringLiteral("first_page")).toInt(), 2);
    QCOMPARE(segments.at(0).toObject().value(QStringLiteral("last_page")).toInt(), 3);
    QCOMPARE(segments.at(0).toObject().value(QStringLiteral("source_path")).toString(),
             QStringLiteral("record.md"));

    QPdfDocument single_pdf;
    QCOMPARE(single_pdf.load(QDir(output).filePath(QStringLiteral("pdfs/a-alpha.pdf"))),
             QPdfDocument::Error::None);
    QCOMPARE(single_pdf.pageCount(), 2);
    QVERIFY(single_pdf.getAllText(0).text().contains(QStringLiteral("Searchable alpha first")));
    QVERIFY(single_pdf.getAllText(1).text().contains(QStringLiteral("Excluded omega second")));
    QVERIFY(!single_pdf.getAllText(0).text().contains(QStringLiteral("JA10")));

    QPdfDocument composite_pdf;
    QCOMPARE(composite_pdf.load(QDir(output).filePath(QStringLiteral("pdfs/z-appendix.pdf"))),
             QPdfDocument::Error::None);
    QCOMPARE(composite_pdf.pageCount(), 5);
    QVERIFY(composite_pdf.getAllText(0).text().contains(QStringLiteral("Searchable cover")));
    QVERIFY(composite_pdf.getAllText(1).text().contains(QStringLiteral("Searchable index")));
    QVERIFY(composite_pdf.getAllText(2).text().contains(QStringLiteral("Searchable green")));
    QVERIFY(composite_pdf.getAllText(3).text().contains(QStringLiteral("Searchable blue")));
    QVERIFY(composite_pdf.getAllText(4).text().contains(QStringLiteral("Searchable alpha")));
    for (int page = 0; page < composite_pdf.pageCount(); ++page) {
        const auto text = composite_pdf.getAllText(page).text();
        QVERIFY2(text.contains(QStringLiteral("JA%1").arg(page + 10)), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("Excluded red")));
        QVERIFY(!text.contains(QStringLiteral("Excluded omega")));
    }
}

void RenderCliTest::repeatsSemanticIdentityWithoutPromisingPdfBytes() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    const auto source = QDir(fixture.source_root).filePath(QStringLiteral("repeat.md"));
    QVERIFY(writeBytes(source, QByteArrayLiteral("# Repeatable\n\nSemantic identity.\n")));
    auto repeated_entry = singleEntry(QStringLiteral("repeat.md"), QStringLiteral("repeat.pdf"),
                                      QStringLiteral("Repeatable Record"));
    repeated_entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("APP"));
    repeated_entry.insert(QStringLiteral("page_label_start"), 7);
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{repeated_entry})));
    const auto first_output = fixture.root.filePath(QStringLiteral("first"));
    const auto second_output = fixture.root.filePath(QStringLiteral("second"));

    const auto first =
        runRenderCli(invocation(fixture.plan_path, fixture.source_root, first_output));
    const auto second =
        runRenderCli(invocation(fixture.plan_path, fixture.source_root, second_output));
    QCOMPARE(first.exit_code, static_cast<int>(RenderCliExitCode::Success));
    QCOMPARE(second.exit_code, static_cast<int>(RenderCliExitCode::Success));
    const auto first_inventory = inventoryAt(first_output);
    const auto second_inventory = inventoryAt(second_output);
    const auto first_entry = entryAt(first_inventory, 0);
    const auto second_entry = entryAt(second_inventory, 0);
    for (const auto& field :
         {QStringLiteral("source_sha256"), QStringLiteral("semantic_render_sha256"),
          QStringLiteral("semantic_plan_sha256"), QStringLiteral("assembly_plan_sha256"),
          QStringLiteral("renderer_provenance")}) {
        QCOMPARE(first_entry.value(field), second_entry.value(field));
    }
    QCOMPARE(first_inventory.value(QStringLiteral("plan_sha256")),
             second_inventory.value(QStringLiteral("plan_sha256")));
    QCOMPARE(first_entry.value(QStringLiteral("pdf_byte_deterministic")).toBool(), false);
    QCOMPARE(first_entry.value(QStringLiteral("pdf_sha256")).toString(),
             fileSha256(QDir(first_output).filePath(QStringLiteral("repeat.pdf"))));
    QCOMPARE(second_entry.value(QStringLiteral("pdf_sha256")).toString(),
             fileSha256(QDir(second_output).filePath(QStringLiteral("repeat.pdf"))));
    QCOMPARE(first_entry.value(QStringLiteral("page_labels")).toObject(),
             QJsonObject({{QStringLiteral("first_number"), 7},
                          {QStringLiteral("last_number"), 7},
                          {QStringLiteral("prefix"), QStringLiteral("APP")}}));
    QPdfDocument repeated_pdf;
    QCOMPARE(repeated_pdf.load(QDir(first_output).filePath(QStringLiteral("repeat.pdf"))),
             QPdfDocument::Error::None);
    QVERIFY(repeated_pdf.getAllText(0).text().contains(QStringLiteral("APP7")));

    repeated_entry.insert(QStringLiteral("page_label_start"), 8);
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{repeated_entry})));
    const auto changed_output = fixture.root.filePath(QStringLiteral("changed"));
    const auto changed =
        runRenderCli(invocation(fixture.plan_path, fixture.source_root, changed_output));
    QCOMPARE(changed.exit_code, static_cast<int>(RenderCliExitCode::Success));
    const auto changed_entry = entryAt(inventoryAt(changed_output), 0);
    QVERIFY(changed_entry.value(QStringLiteral("semantic_render_sha256")) !=
            first_entry.value(QStringLiteral("semantic_render_sha256")));
    QVERIFY(changed_entry.value(QStringLiteral("semantic_plan_sha256")) !=
            first_entry.value(QStringLiteral("semantic_plan_sha256")));

    repeated_entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("JA"));
    repeated_entry.insert(QStringLiteral("page_label_start"), 7);
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{repeated_entry})));
    const auto changed_prefix_output = fixture.root.filePath(QStringLiteral("changed-prefix"));
    const auto changed_prefix =
        runRenderCli(invocation(fixture.plan_path, fixture.source_root, changed_prefix_output));
    QCOMPARE(changed_prefix.exit_code, static_cast<int>(RenderCliExitCode::Success));
    const auto changed_prefix_entry = entryAt(inventoryAt(changed_prefix_output), 0);
    QVERIFY(changed_prefix_entry.value(QStringLiteral("semantic_render_sha256")) !=
            first_entry.value(QStringLiteral("semantic_render_sha256")));
    QVERIFY(changed_prefix_entry.value(QStringLiteral("semantic_plan_sha256")) !=
            first_entry.value(QStringLiteral("semantic_plan_sha256")));
    QVERIFY(!MarkdownPdfRenderer::byteOutputIsDeterministic());
}

void RenderCliTest::rollsBackTheWholeBatchWhenOneRenderFails() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("valid.md")),
                       QByteArrayLiteral("# Valid\n\nThis PDF renders first.\n")));
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("invalid.md")),
                       QByteArrayLiteral("[Forbidden remote link](https://example.invalid/)")));
    QVERIFY(writeJson(fixture.plan_path,
                      planWith(QJsonArray{
                          singleEntry(QStringLiteral("valid.md"), QStringLiteral("a-valid.pdf"),
                                      QStringLiteral("Valid First")),
                          singleEntry(QStringLiteral("invalid.md"), QStringLiteral("z-invalid.pdf"),
                                      QStringLiteral("Invalid Second")),
                      })));
    const auto output = fixture.root.filePath(QStringLiteral("publish"));

    const auto result = runRenderCli(invocation(fixture.plan_path, fixture.source_root, output));
    QCOMPARE(result.exit_code, static_cast<int>(RenderCliExitCode::RenderFailed));
    QCOMPARE(errorCode(result), QStringLiteral("unsupported_markdown_content"));
    QVERIFY(!QFileInfo::exists(output));
    const auto leftovers =
        QDir(fixture.root.path())
            .entryList(QStringList{QStringLiteral(".publish.appellate-render-*")},
                       QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(leftovers.isEmpty());
}

void RenderCliTest::rejectsOverwriteTraversalSymlinksDuplicatesAndUnknownFields() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("one.md")),
                       QByteArrayLiteral("# One\n")));
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("two.md")),
                       QByteArrayLiteral("# Two\n")));

    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{singleEntry(QStringLiteral("one.md"),
                                                                         QStringLiteral("one.pdf"),
                                                                         QStringLiteral("One"))})));
    const auto existing_output = fixture.root.filePath(QStringLiteral("existing"));
    QVERIFY(QDir{}.mkpath(existing_output));
    const auto sentinel = QDir(existing_output).filePath(QStringLiteral("sentinel"));
    QVERIFY(writeBytes(sentinel, QByteArrayLiteral("preserve")));
    auto result = runRenderCli(invocation(fixture.plan_path, fixture.source_root, existing_output));
    QCOMPARE(errorCode(result), QStringLiteral("destination_exists"));
    QFile sentinel_file(sentinel);
    QVERIFY(sentinel_file.open(QIODevice::ReadOnly));
    QCOMPARE(sentinel_file.readAll(), QByteArrayLiteral("preserve"));

    QVERIFY(writeJson(
        fixture.plan_path,
        planWith(QJsonArray{singleEntry(QStringLiteral("../escape.md"),
                                        QStringLiteral("escape.pdf"), QStringLiteral("Escape"))})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("traversal"))));
    QCOMPARE(errorCode(result), QStringLiteral("unsafe_source_path"));

    QVERIFY(writeJson(fixture.plan_path,
                      planWith(QJsonArray{
                          singleEntry(QStringLiteral("one.md"), QStringLiteral("same.pdf"),
                                      QStringLiteral("One")),
                          singleEntry(QStringLiteral("two.md"), QStringLiteral("SAME.pdf"),
                                      QStringLiteral("Two")),
                      })));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("duplicates"))));
    QCOMPARE(errorCode(result), QStringLiteral("duplicate_output_path"));

    auto unknown = singleEntry(QStringLiteral("one.md"), QStringLiteral("unknown.pdf"),
                               QStringLiteral("Unknown"));
    unknown.insert(QStringLiteral("network_url"), QStringLiteral("https://example.invalid"));
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{unknown})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("unknown"))));
    QCOMPARE(errorCode(result), QStringLiteral("unknown_field"));

    const QByteArray duplicate_key_plan =
        QByteArrayLiteral("{\"schema_version\":1,\"schema_version\":1,\"entries\":[]}");
    QVERIFY(writeBytes(fixture.plan_path, duplicate_key_plan));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("duplicate-key"))));
    QCOMPARE(errorCode(result), QStringLiteral("duplicate_json_key"));

#if defined(Q_OS_UNIX)
    const auto linked = QDir(fixture.source_root).filePath(QStringLiteral("linked.md"));
    QVERIFY(QFile::link(QDir(fixture.source_root).filePath(QStringLiteral("one.md")), linked));
    QVERIFY(QFileInfo(linked).isSymLink());
    QVERIFY(writeJson(
        fixture.plan_path,
        planWith(QJsonArray{singleEntry(QStringLiteral("linked.md"), QStringLiteral("linked.pdf"),
                                        QStringLiteral("Linked"))})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("symlink"))));
    QCOMPARE(errorCode(result), QStringLiteral("unsafe_source_path"));
#endif
}

void RenderCliTest::rejectsInvalidCompositeRangesAndExactOneOfViolations() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("pages.md")),
                       QByteArrayLiteral("# Only one page\n")));
    const QJsonArray segments{QJsonObject{
        {QStringLiteral("first_page"), 1},
        {QStringLiteral("last_page"), 2},
        {QStringLiteral("source_path"), QStringLiteral("pages.md")},
    }};
    const QJsonObject ranged{
        {QStringLiteral("output_path"), QStringLiteral("range.pdf")},
        {QStringLiteral("segments"), segments},
        {QStringLiteral("title"), QStringLiteral("Bad Range")},
    };
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{ranged})));
    auto result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                          fixture.root.filePath(QStringLiteral("range"))));
    QCOMPARE(errorCode(result), QStringLiteral("invalid_page_range"));

    auto both = ranged;
    both.insert(QStringLiteral("source_path"), QStringLiteral("pages.md"));
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{both})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("both"))));
    QCOMPARE(errorCode(result), QStringLiteral("schema_violation"));

    auto neither = ranged;
    neither.remove(QStringLiteral("segments"));
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{neither})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("neither"))));
    QCOMPARE(errorCode(result), QStringLiteral("schema_violation"));
}

void RenderCliTest::validatesPageLabelConfigurationAndOverflow() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    const QByteArray two_pages =
        QByteArrayLiteral("# First\n\n") + QByteArray(marker) + QByteArrayLiteral("\n\n# Second\n");
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("pages.md")), two_pages));

    const auto resultFor = [&](QJsonObject entry, QString output_name) {
        if (!writeJson(fixture.plan_path, planWith(QJsonArray{std::move(entry)}))) {
            return RenderCliResult{static_cast<int>(RenderCliExitCode::OperationFailed), {}, {}};
        }
        return runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                       fixture.root.filePath(std::move(output_name))));
    };

    auto entry = singleEntry(QStringLiteral("pages.md"), QStringLiteral("pages.pdf"),
                             QStringLiteral("Pages"));
    entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("JA"));
    auto result = resultFor(entry, QStringLiteral("missing-start"));
    QCOMPARE(errorCode(result), QStringLiteral("schema_violation"));

    entry.insert(QStringLiteral("page_label_start"), 1);
    entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("Ja"));
    result = resultFor(entry, QStringLiteral("lowercase"));
    QCOMPARE(errorCode(result), QStringLiteral("invalid_page_labels"));

    entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("ABCDEFGHIJKLMNOPQ"));
    result = resultFor(entry, QStringLiteral("long-prefix"));
    QCOMPARE(errorCode(result), QStringLiteral("invalid_page_labels"));

    entry.insert(QStringLiteral("page_label_prefix"), QStringLiteral("JA"));
    entry.insert(QStringLiteral("page_label_start"), 0);
    result = resultFor(entry, QStringLiteral("zero-start"));
    QCOMPARE(errorCode(result), QStringLiteral("schema_violation"));

    entry.insert(QStringLiteral("page_label_start"),
                 static_cast<qint64>(appellate::content::MarkdownPdfPageLabels::maximum_number) +
                     1);
    result = resultFor(entry, QStringLiteral("too-high-start"));
    QCOMPARE(errorCode(result), QStringLiteral("schema_violation"));

    entry.insert(QStringLiteral("page_label_start"),
                 appellate::content::MarkdownPdfPageLabels::maximum_number);
    result = resultFor(entry, QStringLiteral("overflow"));
    QCOMPARE(errorCode(result), QStringLiteral("page_label_overflow"));
    QVERIFY(!QFileInfo::exists(fixture.root.filePath(QStringLiteral("overflow"))));
}

void RenderCliTest::enforcesEntryStringSourceInlineAndOutputLimits() {
    Fixture fixture;
    QVERIFY(fixture.root.isValid());
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("one.md")),
                       QByteArrayLiteral("# One source body\n")));
    QVERIFY(writeBytes(QDir(fixture.source_root).filePath(QStringLiteral("two.md")),
                       QByteArrayLiteral("# Two source body\n")));
    const auto two_entries = planWith(QJsonArray{
        singleEntry(QStringLiteral("one.md"), QStringLiteral("one.pdf"), QStringLiteral("One")),
        singleEntry(QStringLiteral("two.md"), QStringLiteral("two.pdf"), QStringLiteral("Two")),
    });
    QVERIFY(writeJson(fixture.plan_path, two_entries));
    RenderBatchLimits limits;
    limits.maximum_entries = 1;
    auto result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                          fixture.root.filePath(QStringLiteral("entry-limit"))),
                               limits);
    QCOMPARE(errorCode(result), QStringLiteral("limit_exceeded"));

    limits = {};
    limits.maximum_path_bytes = 5;
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{singleEntry(QStringLiteral("one.md"),
                                                                         QStringLiteral("one.pdf"),
                                                                         QStringLiteral("One"))})));
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("string-limit"))),
                          limits);
    QCOMPARE(errorCode(result), QStringLiteral("unsafe_output_path"));

    limits = {};
    limits.maximum_total_source_bytes = 4;
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("source-limit"))),
                          limits);
    QCOMPARE(errorCode(result), QStringLiteral("limit_exceeded"));

    const QJsonObject inline_entry{
        {QStringLiteral("front_matter_markdown"), QStringLiteral("# Too much inline text")},
        {QStringLiteral("output_path"), QStringLiteral("inline.pdf")},
        {QStringLiteral("segments"), QJsonArray{QJsonObject{
                                         {QStringLiteral("first_page"), 1},
                                         {QStringLiteral("last_page"), 1},
                                         {QStringLiteral("source_path"), QStringLiteral("one.md")},
                                     }}},
        {QStringLiteral("title"), QStringLiteral("Inline")},
    };
    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{inline_entry})));
    limits = {};
    limits.maximum_inline_markdown_bytes = 5;
    result = runRenderCli(invocation(fixture.plan_path, fixture.source_root,
                                     fixture.root.filePath(QStringLiteral("inline-limit"))),
                          limits);
    QCOMPARE(errorCode(result), QStringLiteral("invalid_string"));

    QVERIFY(writeJson(fixture.plan_path, planWith(QJsonArray{singleEntry(QStringLiteral("one.md"),
                                                                         QStringLiteral("one.pdf"),
                                                                         QStringLiteral("One"))})));
    limits = {};
    limits.maximum_total_output_bytes = 256;
    const auto output_limited = fixture.root.filePath(QStringLiteral("output-limit"));
    result =
        runRenderCli(invocation(fixture.plan_path, fixture.source_root, output_limited), limits);
    QCOMPARE(errorCode(result), QStringLiteral("total_output_limit_exceeded"));
    QVERIFY(!QFileInfo::exists(output_limited));
}

QTEST_MAIN(RenderCliTest)

#include "tst_render_cli.moc"
