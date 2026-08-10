#include "appellate/content/markdown_pdf_renderer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPageSize>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using appellate::content::MarkdownPdfErrorCode;
using appellate::content::MarkdownPdfLimits;
using appellate::content::MarkdownPdfMetadata;
using appellate::content::MarkdownPdfPageLabels;
using appellate::content::MarkdownPdfRenderer;

namespace {

[[nodiscard]] QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] QByteArray twoPageMarkdown() {
    return QByteArrayLiteral("# Synthetic Order\n\n"
                             "Searchable alpha record text appears on the first page.\n\n"
                             "<!-- PAGE BREAK -->\n\n"
                             "## Disposition\n\n"
                             "Searchable omega mandate text appears on the second page.\n");
}

} // namespace

class MarkdownPdfRendererTest final : public QObject {
    Q_OBJECT

  private slots:
    void rendersRealMultipageSearchablePdf();
    void rendersSearchableRepeatablePageLabelsInReservedFooter();
    void semanticDigestIsRepeatableAndByteContractIsHonest();
    void rejectsMalformedAndOversizeInput();
    void rejectsInvalidPageLabelsAndOverflow();
    void refusesOverwriteAndUnsafePaths();
    void rejectsSymlinkSourceAndExternalResources();
    void pageAndOutputBoundsLeaveNoPartialFile();
};

void MarkdownPdfRendererTest::rendersRealMultipageSearchablePdf() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto output_path = directory.filePath(QStringLiteral("synthetic-order.pdf"));

    const MarkdownPdfRenderer renderer;
    const auto result =
        renderer.render(twoPageMarkdown(), output_path,
                        MarkdownPdfMetadata{QStringLiteral("Synthetic Order and Mandate")});
    if (!result) {
        QFAIL(qPrintable(result.error().message));
    }
    QCOMPARE(result->page_count, 2);
    QVERIFY(result->output_bytes > 1'000);
    QCOMPARE(result->pdf_sha256, fileSha256(output_path));
    QCOMPARE(result->source_sha256.size(), 64);
    QCOMPARE(result->semantic_render_sha256.size(), 64);
    QVERIFY(result->renderer_provenance.contains(
        QStringLiteral("contract=appellate.markdown-pdf.semantic-layout.v2")));
    QVERIFY(result->renderer_provenance.contains(QStringLiteral("qt_runtime_version=")));
    QVERIFY(result->renderer_provenance.contains(QStringLiteral("margins=54pt-all")));
    QVERIFY(result->renderer_provenance.contains(
        QStringLiteral("pdf_byte_determinism=not-guaranteed")));

    QFile bytes(output_path);
    QVERIFY(bytes.open(QIODevice::ReadOnly));
    QCOMPARE(bytes.read(5), QByteArrayLiteral("%PDF-"));
    QCOMPARE(bytes.size(), result->output_bytes);

    QPdfDocument document;
    QCOMPARE(document.load(output_path), QPdfDocument::Error::None);
    QCOMPARE(document.status(), QPdfDocument::Status::Ready);
    QCOMPARE(document.pageCount(), 2);
    QCOMPARE(document.metaData(QPdfDocument::MetaDataField::Title).toString(),
             QStringLiteral("Synthetic Order and Mandate"));
    QCOMPARE(document.metaData(QPdfDocument::MetaDataField::Author).toString(),
             QStringLiteral("Appellate Workbench synthetic content"));

    const auto first_page_text = document.getAllText(0).text();
    const auto second_page_text = document.getAllText(1).text();
    QVERIFY2(first_page_text.contains(QStringLiteral("Searchable alpha record text")),
             qPrintable(first_page_text));
    QVERIFY2(second_page_text.contains(QStringLiteral("Searchable omega mandate text")),
             qPrintable(second_page_text));
    QVERIFY(!first_page_text.contains(QStringLiteral("JA1")));
    QVERIFY(!second_page_text.contains(QStringLiteral("JA2")));
    QVERIFY(!result->renderer_provenance.contains(QStringLiteral("page_label_contract=")));

    const QPageSize letter(QPageSize::Letter);
    const auto expected_size = letter.size(QPageSize::Point);
    const auto actual_size = document.pagePointSize(0);
    QVERIFY(qAbs(actual_size.width() - expected_size.width()) < 0.1);
    QVERIFY(qAbs(actual_size.height() - expected_size.height()) < 0.1);
}

void MarkdownPdfRendererTest::rendersSearchableRepeatablePageLabelsInReservedFooter() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MarkdownPdfMetadata metadata{QStringLiteral("Labeled Joint Appendix")};
    metadata.page_labels = MarkdownPdfPageLabels{QStringLiteral("JA"), 41};

    const MarkdownPdfRenderer renderer;
    const auto first = renderer.render(
        twoPageMarkdown(), directory.filePath(QStringLiteral("labeled-first.pdf")), metadata);
    if (!first) {
        QFAIL(qPrintable(first.error().message));
    }
    const auto second = renderer.render(
        twoPageMarkdown(), directory.filePath(QStringLiteral("labeled-second.pdf")), metadata);
    if (!second) {
        QFAIL(qPrintable(second.error().message));
    }
    QCOMPARE(first->semantic_render_sha256, second->semantic_render_sha256);
    QCOMPARE(first->renderer_provenance, second->renderer_provenance);
    QVERIFY(first->renderer_provenance.contains(QStringLiteral("page_label_prefix=JA\n")));
    QVERIFY(first->renderer_provenance.contains(QStringLiteral("page_label_first_number=41\n")));
    QVERIFY(first->renderer_provenance.contains(QStringLiteral("page_label_last_number=42\n")));
    QVERIFY(first->renderer_provenance.contains(
        QStringLiteral("page_label_footer_band=28pt-reserved-inside-paint-rectangle")));

    const auto unlabeled =
        renderer.render(twoPageMarkdown(), directory.filePath(QStringLiteral("unlabeled.pdf")),
                        MarkdownPdfMetadata{QStringLiteral("Labeled Joint Appendix")});
    if (!unlabeled) {
        QFAIL(qPrintable(unlabeled.error().message));
    }
    QVERIFY(first->semantic_render_sha256 != unlabeled->semantic_render_sha256);

    QPdfDocument document;
    QCOMPARE(document.load(directory.filePath(QStringLiteral("labeled-first.pdf"))),
             QPdfDocument::Error::None);
    QCOMPARE(document.pageCount(), 2);
    const QStringList expected_labels{QStringLiteral("JA41"), QStringLiteral("JA42")};
    for (int page = 0; page < document.pageCount(); ++page) {
        const auto all_text = document.getAllText(page).text();
        const auto& expected_label = expected_labels.at(page);
        const auto label_index = all_text.indexOf(expected_label);
        QVERIFY2(label_index >= 0, qPrintable(all_text));
        const auto label_selection = document.getSelectionAtIndex(
            page, static_cast<int>(label_index), static_cast<int>(expected_label.size()));
        QVERIFY(label_selection.isValid());
        QVERIFY2(label_selection.boundingRectangle().top() >= 710.0,
                 qPrintable(QStringLiteral("Label bounds unexpectedly enter body at y=%1")
                                .arg(label_selection.boundingRectangle().top())));
        QVERIFY2(
            label_selection.boundingRectangle().bottom() <= 738.0,
            qPrintable(QStringLiteral("Label bounds unexpectedly leave paint rectangle at y=%1")
                           .arg(label_selection.boundingRectangle().bottom())));
    }
}

void MarkdownPdfRendererTest::semanticDigestIsRepeatableAndByteContractIsHonest() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto first_path = directory.filePath(QStringLiteral("first.pdf"));
    const auto second_path = directory.filePath(QStringLiteral("second.pdf"));
    const MarkdownPdfMetadata metadata{QStringLiteral("Repeatable Semantic Render")};

    const MarkdownPdfRenderer renderer;
    const auto first = renderer.render(twoPageMarkdown(), first_path, metadata);
    if (!first) {
        QFAIL(qPrintable(first.error().message));
    }
    const auto second = renderer.render(twoPageMarkdown(), second_path, metadata);
    if (!second) {
        QFAIL(qPrintable(second.error().message));
    }

    QCOMPARE(first->source_sha256, second->source_sha256);
    QCOMPARE(first->semantic_render_sha256, second->semantic_render_sha256);
    QCOMPARE(first->renderer_provenance, second->renderer_provenance);
    QCOMPARE(first->page_count, second->page_count);
    QCOMPARE(first->pdf_sha256, fileSha256(first_path));
    QCOMPARE(second->pdf_sha256, fileSha256(second_path));
    QVERIFY(!MarkdownPdfRenderer::byteOutputIsDeterministic());

    const auto changed_title =
        renderer.render(twoPageMarkdown(), directory.filePath(QStringLiteral("changed-title.pdf")),
                        MarkdownPdfMetadata{QStringLiteral("Different Semantic Metadata")});
    if (!changed_title) {
        QFAIL(qPrintable(changed_title.error().message));
    }
    QVERIFY(changed_title->semantic_render_sha256 != first->semantic_render_sha256);
}

void MarkdownPdfRendererTest::rejectsMalformedAndOversizeInput() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QByteArray malformed;
    malformed.append(static_cast<char>(0xC3));
    malformed.append(static_cast<char>(0x28));
    const MarkdownPdfRenderer renderer;
    const auto malformed_result =
        renderer.render(malformed, directory.filePath(QStringLiteral("malformed.pdf")));
    QVERIFY(!malformed_result.has_value());
    QCOMPARE(malformed_result.error().code, MarkdownPdfErrorCode::InvalidUtf8);
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("malformed.pdf"))));

    const MarkdownPdfRenderer bounded(
        MarkdownPdfLimits{16, MarkdownPdfLimits::default_max_output_bytes, 10});
    const QByteArray oversized(17, 'x');
    const auto oversized_result =
        bounded.render(oversized, directory.filePath(QStringLiteral("oversized.pdf")));
    QVERIFY(!oversized_result.has_value());
    QCOMPARE(oversized_result.error().code, MarkdownPdfErrorCode::InputTooLarge);
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("oversized.pdf"))));

    QByteArray controls = QByteArrayLiteral("valid");
    controls.append('\0');
    controls.append(QByteArrayLiteral("text"));
    const auto control_result =
        renderer.render(controls, directory.filePath(QStringLiteral("control.pdf")));
    QVERIFY(!control_result.has_value());
    QCOMPARE(control_result.error().code, MarkdownPdfErrorCode::InvalidUtf8);
}

void MarkdownPdfRendererTest::rejectsInvalidPageLabelsAndOverflow() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const MarkdownPdfRenderer renderer;

    const auto rejects = [&](QString file_name, MarkdownPdfPageLabels labels) {
        MarkdownPdfMetadata metadata{QStringLiteral("Invalid Page Labels")};
        metadata.page_labels = std::move(labels);
        const auto result =
            renderer.render(twoPageMarkdown(), directory.filePath(file_name), metadata);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().code, MarkdownPdfErrorCode::InvalidConfiguration);
        QVERIFY(!QFileInfo::exists(directory.filePath(file_name)));
    };
    rejects(QStringLiteral("empty-prefix.pdf"), MarkdownPdfPageLabels{{}, 1});
    rejects(QStringLiteral("lowercase-prefix.pdf"), MarkdownPdfPageLabels{QStringLiteral("Ja"), 1});
    rejects(QStringLiteral("numeric-prefix.pdf"), MarkdownPdfPageLabels{QStringLiteral("JA1"), 1});
    rejects(QStringLiteral("long-prefix.pdf"),
            MarkdownPdfPageLabels{QStringLiteral("ABCDEFGHIJKLMNOPQ"), 1});
    rejects(QStringLiteral("zero-start.pdf"), MarkdownPdfPageLabels{QStringLiteral("JA"), 0});
    rejects(QStringLiteral("overflow.pdf"),
            MarkdownPdfPageLabels{QStringLiteral("JA"), MarkdownPdfPageLabels::maximum_number});
}

void MarkdownPdfRendererTest::refusesOverwriteAndUnsafePaths() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto existing_path = directory.filePath(QStringLiteral("existing.pdf"));
    QFile existing(existing_path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("preserve-me"), 11);
    existing.close();

    const MarkdownPdfRenderer renderer;
    const auto overwrite = renderer.render(twoPageMarkdown(), existing_path);
    QVERIFY(!overwrite.has_value());
    QCOMPARE(overwrite.error().code, MarkdownPdfErrorCode::OutputAlreadyExists);
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArrayLiteral("preserve-me"));

    const auto relative = renderer.render(twoPageMarkdown(), QStringLiteral("relative.pdf"));
    QVERIFY(!relative.has_value());
    QCOMPARE(relative.error().code, MarkdownPdfErrorCode::UnsafeOutputPath);

    const auto traversal_path = directory.path() + QStringLiteral("/sub/../traversal.pdf");
    const auto traversal = renderer.render(twoPageMarkdown(), traversal_path);
    QVERIFY(!traversal.has_value());
    QCOMPARE(traversal.error().code, MarkdownPdfErrorCode::UnsafeOutputPath);

#if defined(Q_OS_UNIX)
    const auto symlink_path = directory.filePath(QStringLiteral("linked.pdf"));
    QVERIFY(QFile::link(existing_path, symlink_path));
    const auto symlink = renderer.render(twoPageMarkdown(), symlink_path);
    QVERIFY(!symlink.has_value());
    QCOMPARE(symlink.error().code, MarkdownPdfErrorCode::UnsafeOutputPath);
#endif
}

void MarkdownPdfRendererTest::rejectsSymlinkSourceAndExternalResources() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto source_path = directory.filePath(QStringLiteral("source.md"));
    QFile source(source_path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("# Safe source\n"), 14);
    source.close();

    const MarkdownPdfRenderer renderer;
    const auto direct_result =
        renderer.renderFile(source_path, directory.filePath(QStringLiteral("direct.pdf")));
    if (!direct_result) {
        QFAIL(qPrintable(direct_result.error().message));
    }
    QCOMPARE(direct_result->page_count, 1);
    QCOMPARE(direct_result->pdf_sha256,
             fileSha256(directory.filePath(QStringLiteral("direct.pdf"))));
#if defined(Q_OS_UNIX)
    const auto linked_source = directory.filePath(QStringLiteral("linked.md"));
    QVERIFY(QFile::link(source_path, linked_source));
    const auto linked_result =
        renderer.renderFile(linked_source, directory.filePath(QStringLiteral("linked.pdf")));
    QVERIFY(!linked_result.has_value());
    QCOMPARE(linked_result.error().code, MarkdownPdfErrorCode::UnsafeSourcePath);
#endif

    const auto external =
        renderer.render(QByteArrayLiteral("![remote exhibit](https://example.invalid/exhibit.png)"),
                        directory.filePath(QStringLiteral("external.pdf")));
    QVERIFY(!external.has_value());
    QCOMPARE(external.error().code, MarkdownPdfErrorCode::UnsupportedContent);
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("external.pdf"))));
}

void MarkdownPdfRendererTest::pageAndOutputBoundsLeaveNoPartialFile() {
    QTemporaryDir page_directory;
    QVERIFY(page_directory.isValid());
    const MarkdownPdfRenderer one_page(
        MarkdownPdfLimits{MarkdownPdfLimits::default_max_input_bytes,
                          MarkdownPdfLimits::default_max_output_bytes, 1});
    const auto page_limited = one_page.render(
        twoPageMarkdown(), page_directory.filePath(QStringLiteral("too-many-pages.pdf")));
    QVERIFY(!page_limited.has_value());
    QCOMPARE(page_limited.error().code, MarkdownPdfErrorCode::PageLimitExceeded);
    QVERIFY(QDir(page_directory.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());

    QTemporaryDir output_directory;
    QVERIFY(output_directory.isValid());
    const MarkdownPdfRenderer tiny_output(
        MarkdownPdfLimits{MarkdownPdfLimits::default_max_input_bytes, 256, 10});
    const auto output_limited = tiny_output.render(
        twoPageMarkdown(), output_directory.filePath(QStringLiteral("too-large.pdf")));
    QVERIFY(!output_limited.has_value());
    QCOMPARE(output_limited.error().code, MarkdownPdfErrorCode::OutputTooLarge);
    QVERIFY(QDir(output_directory.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

QTEST_MAIN(MarkdownPdfRendererTest)

#include "tst_markdown_pdf_renderer.moc"
