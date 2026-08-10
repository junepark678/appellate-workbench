#include "appellate/content/markdown_pdf_renderer.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStringDecoder>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextFragment>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace appellate::content {
namespace {

constexpr qint64 hard_max_input_bytes = 16LL * 1024LL * 1024LL;
constexpr qint64 hard_max_output_bytes = 512LL * 1024LL * 1024LL;
constexpr int hard_max_pages = 2'048;
constexpr int pdf_resolution_dpi = 72;
constexpr qreal page_margin_points = 54.0;
constexpr qreal body_font_points = 10.0;
constexpr qreal monospace_font_points = 9.0;
constexpr qreal page_label_font_points = 9.0;
constexpr qreal page_label_footer_band_points = 28.0;
constexpr qreal page_label_top_gap_points = 4.0;
constexpr qreal page_label_bottom_inset_points = 4.0;

constexpr auto page_break_marker = "<!-- PAGE BREAK -->";
constexpr auto renderer_contract = "appellate.markdown-pdf.semantic-layout.v2";
constexpr auto fixed_author = "Appellate Workbench synthetic content";
constexpr auto fixed_creator = "Appellate Workbench Markdown PDF Renderer";
constexpr auto fixed_xmp_timestamp = "1970-01-01T00:00:00Z";
constexpr auto body_font_request = "serif";
constexpr auto monospace_font_request = "monospace";

constexpr auto default_style_sheet = R"CSS(
body {
  background-color: #ffffff;
  color: #000000;
  font-family: serif;
  font-size: 10pt;
  line-height: 1.22;
}
h1 { font-size: 15pt; font-weight: 700; margin: 12pt 0 8pt 0; }
h2 { font-size: 13pt; font-weight: 700; margin: 10pt 0 6pt 0; }
h3, h4, h5, h6 { font-size: 11pt; font-weight: 700; margin: 8pt 0 4pt 0; }
p { margin: 0 0 7pt 0; }
blockquote { margin: 5pt 18pt; }
pre, code {
  font-family: monospace;
  font-size: 9pt;
}
table { border-collapse: collapse; }
th, td { border: 0.5pt solid #000000; padding: 2pt; }
a { color: #000000; text-decoration: none; }
)CSS";

[[nodiscard]] auto fail(MarkdownPdfErrorCode code, QString message)
    -> std::unexpected<MarkdownPdfError> {
    return std::unexpected(MarkdownPdfError{code, std::move(message)});
}

[[nodiscard]] QString lowercaseSha256(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void addDigestFrame(QCryptographicHash& hash, QByteArrayView name, QByteArrayView value) {
    hash.addData(name);
    hash.addData(QByteArrayView("\0", 1));
    const auto size = QByteArray::number(value.size());
    hash.addData(size);
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(value);
    hash.addData(QByteArrayView("\0", 1));
}

[[nodiscard]] bool hasControlCharacters(QStringView value) {
    for (const auto character : value) {
        const auto code = character.unicode();
        if (code < 0x20U || code == 0x7FU) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasUnsupportedMarkdownControls(QStringView markdown) {
    for (const auto character : markdown) {
        const auto code = character.unicode();
        if ((code < 0x20U && character != u'\n' && character != u'\r' && character != u'\t') ||
            code == 0x7FU) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto validateLimits(const MarkdownPdfLimits& limits)
    -> std::expected<void, MarkdownPdfError> {
    if (limits.max_input_bytes <= 0 || limits.max_input_bytes > hard_max_input_bytes) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("The Markdown input limit must be between 1 and %1 bytes")
                        .arg(hard_max_input_bytes));
    }
    if (limits.max_output_bytes <= 0 || limits.max_output_bytes > hard_max_output_bytes) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("The PDF output limit must be between 1 and %1 bytes")
                        .arg(hard_max_output_bytes));
    }
    if (limits.max_pages <= 0 || limits.max_pages > hard_max_pages) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("The page limit must be between 1 and %1").arg(hard_max_pages));
    }
    return {};
}

[[nodiscard]] bool pathHasSymlinkComponent(const QString& absolute_path) {
    auto current = absolute_path;
    while (true) {
        const QFileInfo information(current);
        if (information.isSymLink()) {
            return true;
        }

        const auto parent = information.dir().absolutePath();
        if (parent == current) {
            return false;
        }
        current = parent;
    }
}

[[nodiscard]] bool isCleanAbsolutePath(const QString& path) {
    if (path.isEmpty() || hasControlCharacters(path) || !QDir::isAbsolutePath(path)) {
        return false;
    }
    const auto normalized = QDir::fromNativeSeparators(path);
    if (normalized.endsWith(u'/') || QDir::cleanPath(normalized) != normalized) {
        return false;
    }
    return QFileInfo(normalized).absoluteFilePath() == normalized;
}

[[nodiscard]] auto validateSourcePath(const QString& path)
    -> std::expected<void, MarkdownPdfError> {
    if (!isCleanAbsolutePath(path) || pathHasSymlinkComponent(path)) {
        return fail(MarkdownPdfErrorCode::UnsafeSourcePath,
                    QStringLiteral("Markdown source paths must be clean absolute paths without "
                                   "symbolic-link components"));
    }

    const QFileInfo information(path);
    if (!information.exists()) {
        return fail(MarkdownPdfErrorCode::SourceNotFound,
                    QStringLiteral("Markdown source does not exist: %1").arg(path));
    }
    if (!information.isFile()) {
        return fail(MarkdownPdfErrorCode::SourceNotRegularFile,
                    QStringLiteral("Markdown source is not a regular file: %1").arg(path));
    }
    return {};
}

[[nodiscard]] auto validateOutputPath(const QString& path)
    -> std::expected<void, MarkdownPdfError> {
    if (!isCleanAbsolutePath(path) || !path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive) ||
        pathHasSymlinkComponent(path)) {
        return fail(MarkdownPdfErrorCode::UnsafeOutputPath,
                    QStringLiteral("PDF output must be a clean absolute .pdf path without "
                                   "symbolic-link components"));
    }

    const QFileInfo information(path);
    if (information.exists() || information.isSymLink()) {
        return fail(MarkdownPdfErrorCode::OutputAlreadyExists,
                    QStringLiteral("Refusing to overwrite PDF output: %1").arg(path));
    }

    const QFileInfo parent(information.dir().absolutePath());
    if (!parent.exists() || !parent.isDir() || parent.isSymLink()) {
        return fail(MarkdownPdfErrorCode::UnsafeOutputPath,
                    QStringLiteral("PDF output directory must be an existing regular directory"));
    }
    return {};
}

[[nodiscard]] auto validateMetadata(const MarkdownPdfMetadata& metadata)
    -> std::expected<void, MarkdownPdfError> {
    const auto encoded_title = metadata.title.toUtf8();
    if (metadata.title.isEmpty() || metadata.title.trimmed() != metadata.title ||
        encoded_title.size() > 512 || hasControlCharacters(metadata.title)) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("PDF title must be 1-512 UTF-8 bytes with no surrounding "
                                   "whitespace or control characters"));
    }
    if (metadata.page_labels) {
        const auto& labels = *metadata.page_labels;
        const auto encoded_prefix = labels.prefix.toLatin1();
        const auto uppercase_ascii = std::ranges::all_of(
            labels.prefix, [](QChar character) { return character >= u'A' && character <= u'Z'; });
        if (labels.prefix.isEmpty() ||
            encoded_prefix.size() > MarkdownPdfPageLabels::maximum_prefix_bytes ||
            !uppercase_ascii || labels.first_number < 1 ||
            labels.first_number > MarkdownPdfPageLabels::maximum_number) {
            return fail(
                MarkdownPdfErrorCode::InvalidConfiguration,
                QStringLiteral("Page labels require a 1-16 letter uppercase ASCII prefix and a "
                               "first number between 1 and %1")
                    .arg(MarkdownPdfPageLabels::maximum_number));
        }
    }
    return {};
}

[[nodiscard]] QFont configuredFont(const char* generic_family, QFont::StyleHint style_hint,
                                   qreal point_size) {
    QFont font(QString::fromLatin1(generic_family));
    font.setPointSizeF(point_size);
    font.setKerning(false);
    font.setStyleHint(style_hint, QFont::PreferDefault);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

[[nodiscard]] auto resolveFont(const QFont& font, const char* generic_family)
    -> std::expected<QString, MarkdownPdfError> {
    const QFontInfo information(font);
    if (information.family().isEmpty()) {
        return fail(MarkdownPdfErrorCode::RequiredFontUnavailable,
                    QStringLiteral("Cannot resolve the platform %1 font")
                        .arg(QString::fromLatin1(generic_family)));
    }
    return information.family();
}

class IsolatedTextDocument final : public QTextDocument {
  public:
    using QTextDocument::QTextDocument;

  protected:
    [[nodiscard]] QVariant loadResource(int type, const QUrl& name) override {
        Q_UNUSED(type);
        Q_UNUSED(name);
        resource_requested_ = true;
        return {};
    }

  public:
    [[nodiscard]] bool resourceRequested() const noexcept { return resource_requested_; }

  private:
    bool resource_requested_{};
};

[[nodiscard]] bool containsLinkOrImage(const QTextDocument& document) {
    for (auto block = document.begin(); block != document.end(); block = block.next()) {
        for (auto fragment_iterator = block.begin(); !fragment_iterator.atEnd();
             ++fragment_iterator) {
            const auto fragment = fragment_iterator.fragment();
            if (!fragment.isValid()) {
                continue;
            }
            const auto format = fragment.charFormat();
            if (format.isImageFormat() || (format.isAnchor() && !format.anchorHref().isEmpty())) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] QString buildProvenance(QStringView source_sha256, QStringView title_sha256,
                                      QStringView resolved_body_font,
                                      QStringView resolved_monospace_font,
                                      const std::optional<MarkdownPdfPageLabels>& page_labels,
                                      int page_count) {
    const auto css_sha256 = lowercaseSha256(QByteArrayView(default_style_sheet));
    auto provenance =
        QStringLiteral("contract=%1\n"
                       "source_encoding=UTF-8\n"
                       "source_sha256=%2\n"
                       "title_sha256=%3\n"
                       "qt_build_version=%4\n"
                       "qt_runtime_version=%5\n"
                       "qt_build_abi=%6\n"
                       "paper=US-Letter-612x792pt\n"
                       "orientation=portrait\n"
                       "margins=54pt-all\n"
                       "resolution=72dpi\n"
                       "body_font=generic-serif-resolved-as-%7-10pt-no-kerning-no-hinting\n"
                       "monospace_font=generic-monospace-resolved-as-%8-9pt-no-kerning-no-hinting\n"
                       "stylesheet_sha256=%9\n"
                       "markdown=Qt-GitHub-without-raw-HTML\n"
                       "page_break_marker=%10\n"
                       "author=%11\n"
                       "creator=%12\n"
                       "xmp_timestamp=%13\n"
                       "pdf_byte_determinism=not-guaranteed-QPdfWriter-wall-clock-info-dates\n")
            .arg(QString::fromLatin1(renderer_contract), source_sha256, title_sha256,
                 QString::fromLatin1(QT_VERSION_STR), QString::fromLatin1(qVersion()),
                 QSysInfo::buildAbi(), resolved_body_font, resolved_monospace_font, css_sha256,
                 QString::fromLatin1(page_break_marker), QString::fromLatin1(fixed_author),
                 QString::fromLatin1(fixed_creator), QString::fromLatin1(fixed_xmp_timestamp));
    if (page_labels) {
        const auto last_number = static_cast<qint64>(page_labels->first_number) + page_count - 1;
        provenance += QStringLiteral(
                          "page_label_contract=uppercase-ascii-prefix-plus-base10-v1\n"
                          "page_label_prefix=%1\n"
                          "page_label_first_number=%2\n"
                          "page_label_last_number=%3\n"
                          "page_label_number_format=base10-no-leading-zero\n"
                          "page_label_font=generic-serif-resolved-as-%4-9pt-no-kerning-no-hinting\n"
                          "page_label_footer_band=28pt-reserved-inside-paint-rectangle\n"
                          "page_label_body_height=paint-rectangle-height-minus-28pt\n"
                          "page_label_region=footer-band-top-plus-4pt-to-paint-bottom-minus-4pt\n"
                          "page_label_alignment=horizontal-center-vertical-center\n")
                          .arg(page_labels->prefix)
                          .arg(page_labels->first_number)
                          .arg(last_number)
                          .arg(resolved_body_font);
    }
    return provenance;
}

[[nodiscard]] QString semanticRenderDigest(QByteArrayView source, QByteArrayView title,
                                           QByteArrayView provenance) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    addDigestFrame(hash, QByteArrayView("canonical-markdown-utf8"), source);
    addDigestFrame(hash, QByteArrayView("pdf-title-utf8"), title);
    addDigestFrame(hash, QByteArrayView("renderer-provenance-utf8"), provenance);
    return QString::fromLatin1(hash.result().toHex());
}

[[nodiscard]] QUuid deterministicDocumentId(QStringView semantic_digest) {
    auto bytes = QByteArray::fromHex(semantic_digest.left(32).toLatin1());
    if (bytes.size() == 16) {
        bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0FU) | 0x50U);
        bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3FU) | 0x80U);
    }
    return QUuid::fromRfc4122(bytes);
}

[[nodiscard]] QByteArray fixedXmpMetadata(QStringView title, const QUuid& document_id) {
    const auto escaped_title = title.toString().toHtmlEscaped().toUtf8();
    const auto uuid = document_id.toString(QUuid::WithoutBraces).toUtf8();
    QByteArray metadata;
    metadata.reserve(1'024 + escaped_title.size());
    metadata.append("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                    "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
                    "rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                    "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
                    "xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\"><dc:title><rdf:Alt>"
                    "<rdf:li xml:lang=\"x-default\">");
    metadata.append(escaped_title);
    metadata.append("</rdf:li></rdf:Alt></dc:title><dc:creator><rdf:Seq><rdf:li>");
    metadata.append(fixed_author);
    metadata.append("</rdf:li></rdf:Seq></dc:creator><xmp:CreatorTool>");
    metadata.append(fixed_creator);
    metadata.append("</xmp:CreatorTool><xmp:CreateDate>");
    metadata.append(fixed_xmp_timestamp);
    metadata.append("</xmp:CreateDate><xmp:ModifyDate>");
    metadata.append(fixed_xmp_timestamp);
    metadata.append("</xmp:ModifyDate><xmp:MetadataDate>");
    metadata.append(fixed_xmp_timestamp);
    metadata.append("</xmp:MetadataDate><xmpMM:DocumentID>uuid:");
    metadata.append(uuid);
    metadata.append("</xmpMM:DocumentID></rdf:Description></rdf:RDF></x:xmpmeta>");
    return metadata;
}

class BoundedHashingWriteDevice final : public QIODevice {
  public:
    BoundedHashingWriteDevice(QFile& destination, qint64 maximum_bytes)
        : destination_(destination), maximum_bytes_(maximum_bytes) {}

    [[nodiscard]] bool openForWriting() { return QIODevice::open(QIODevice::WriteOnly); }
    [[nodiscard]] bool exceededLimit() const noexcept { return exceeded_limit_; }
    [[nodiscard]] bool writeFailed() const noexcept { return write_failed_; }
    [[nodiscard]] qint64 bytesWritten() const noexcept { return bytes_written_; }
    [[nodiscard]] QString sha256() { return QString::fromLatin1(hash_.result().toHex()); }

    [[nodiscard]] bool isSequential() const override { return true; }

  protected:
    [[nodiscard]] qint64 readData(char* data, qint64 maximum_size) override {
        Q_UNUSED(data);
        Q_UNUSED(maximum_size);
        return -1;
    }

    [[nodiscard]] qint64 writeData(const char* data, qint64 size) override {
        if (size < 0 || size > maximum_bytes_ - bytes_written_) {
            exceeded_limit_ = true;
            return -1;
        }

        const auto written = destination_.write(data, size);
        if (written > 0) {
            hash_.addData(QByteArrayView(data, written));
            bytes_written_ += written;
        }
        if (written != size) {
            write_failed_ = true;
        }
        return written;
    }

  private:
    QFile& destination_;
    qint64 maximum_bytes_{};
    qint64 bytes_written_{};
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    bool exceeded_limit_{};
    bool write_failed_{};
};

[[nodiscard]] bool syncFile(QFileDevice& file) {
    if (!file.flush()) {
        return false;
    }
    const auto handle = file.handle();
    if (handle < 0) {
        return false;
    }
#if defined(Q_OS_UNIX)
    int result{};
    do {
        result = ::fsync(static_cast<int>(handle));
    } while (result != 0 && errno == EINTR);
    return result == 0;
#elif defined(Q_OS_WIN)
    return ::_commit(static_cast<int>(handle)) == 0;
#else
    return true;
#endif
}

struct PreparedDocuments final {
    std::vector<std::unique_ptr<IsolatedTextDocument>> documents;
    int page_count{};
    QString resolved_body_font;
    QString resolved_monospace_font;
};

[[nodiscard]] auto prepareDocuments(QStringView markdown, const MarkdownPdfLimits& limits,
                                    const QSizeF& content_size)
    -> std::expected<PreparedDocuments, MarkdownPdfError> {
    const auto body_font = configuredFont(body_font_request, QFont::Serif, body_font_points);
    const auto monospace_font =
        configuredFont(monospace_font_request, QFont::Monospace, monospace_font_points);
    const auto resolved_body_font = resolveFont(body_font, body_font_request);
    if (!resolved_body_font) {
        return std::unexpected(resolved_body_font.error());
    }
    const auto resolved_monospace_font = resolveFont(monospace_font, monospace_font_request);
    if (!resolved_monospace_font) {
        return std::unexpected(resolved_monospace_font.error());
    }

    const auto segments =
        markdown.toString().split(QString::fromLatin1(page_break_marker), Qt::KeepEmptyParts);
    if (segments.size() > limits.max_pages) {
        return fail(MarkdownPdfErrorCode::PageLimitExceeded,
                    QStringLiteral("Page-break markers exceed the configured %1-page limit")
                        .arg(limits.max_pages));
    }

    PreparedDocuments prepared;
    prepared.resolved_body_font = *resolved_body_font;
    prepared.resolved_monospace_font = *resolved_monospace_font;
    prepared.documents.reserve(static_cast<std::size_t>(segments.size()));
    for (const auto& segment : segments) {
        auto document = std::make_unique<IsolatedTextDocument>();
        document->setUndoRedoEnabled(false);
        document->setUseDesignMetrics(true);
        document->setDocumentMargin(0.0);
        document->setDefaultFont(body_font);
        document->setDefaultStyleSheet(QString::fromLatin1(default_style_sheet));
        document->setPageSize(content_size);
        document->setBaseUrl({});
        auto markdown_features =
            QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub);
        markdown_features.setFlag(QTextDocument::MarkdownNoHTML);
        document->setMarkdown(segment, markdown_features);

        if (containsLinkOrImage(*document) || document->resourceRequested()) {
            return fail(MarkdownPdfErrorCode::UnsupportedContent,
                        QStringLiteral("Images, links, and external resources are forbidden in "
                                       "authored record PDFs"));
        }

        const auto segment_pages = document->pageCount();
        if (segment_pages <= 0 || segment_pages > limits.max_pages - prepared.page_count) {
            return fail(MarkdownPdfErrorCode::PageLimitExceeded,
                        QStringLiteral("Rendered document exceeds the configured %1-page limit")
                            .arg(limits.max_pages));
        }
        prepared.page_count += segment_pages;
        prepared.documents.push_back(std::move(document));
    }
    return prepared;
}

} // namespace

MarkdownPdfRenderer::MarkdownPdfRenderer(MarkdownPdfLimits limits) : limits_(limits) {}

const MarkdownPdfLimits& MarkdownPdfRenderer::limits() const noexcept { return limits_; }

QStringView MarkdownPdfRenderer::pageBreakMarker() noexcept {
    static const QString marker = QString::fromLatin1(page_break_marker);
    return marker;
}

QStringView MarkdownPdfRenderer::rendererContractVersion() noexcept {
    static const QString contract = QString::fromLatin1(renderer_contract);
    return contract;
}

auto MarkdownPdfRenderer::renderFile(QStringView absolute_markdown_path,
                                     QStringView absolute_output_path,
                                     const MarkdownPdfMetadata& metadata) const
    -> std::expected<MarkdownPdfResult, MarkdownPdfError> {
    if (const auto limits = validateLimits(limits_); !limits) {
        return std::unexpected(limits.error());
    }

    const auto source_path = absolute_markdown_path.toString();
    if (const auto source = validateSourcePath(source_path); !source) {
        return std::unexpected(source.error());
    }

    const QFileInfo information(source_path);
    if (information.size() > limits_.max_input_bytes) {
        return fail(MarkdownPdfErrorCode::InputTooLarge,
                    QStringLiteral("Markdown source exceeds the configured %1-byte limit")
                        .arg(limits_.max_input_bytes));
    }

    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(MarkdownPdfErrorCode::CannotReadSource,
                    QStringLiteral("Cannot open Markdown source: %1").arg(source_path));
    }
    const auto bytes = source.read(limits_.max_input_bytes + 1);
    if (bytes.size() > limits_.max_input_bytes) {
        return fail(MarkdownPdfErrorCode::InputTooLarge,
                    QStringLiteral("Markdown source exceeds the configured %1-byte limit")
                        .arg(limits_.max_input_bytes));
    }
    if (!source.atEnd() || source.error() != QFileDevice::NoError) {
        return fail(
            MarkdownPdfErrorCode::CannotReadSource,
            QStringLiteral("Cannot read the complete Markdown source: %1").arg(source_path));
    }
    return render(bytes, absolute_output_path, metadata);
}

auto MarkdownPdfRenderer::render(QByteArrayView utf8_markdown, QStringView absolute_output_path,
                                 const MarkdownPdfMetadata& metadata) const
    -> std::expected<MarkdownPdfResult, MarkdownPdfError> {
    if (const auto limits = validateLimits(limits_); !limits) {
        return std::unexpected(limits.error());
    }
    if (const auto metadata_status = validateMetadata(metadata); !metadata_status) {
        return std::unexpected(metadata_status.error());
    }
    if (utf8_markdown.isEmpty()) {
        return fail(MarkdownPdfErrorCode::EmptyInput,
                    QStringLiteral("Markdown input must not be empty"));
    }
    if (utf8_markdown.size() > limits_.max_input_bytes) {
        return fail(MarkdownPdfErrorCode::InputTooLarge,
                    QStringLiteral("Markdown input exceeds the configured %1-byte limit")
                        .arg(limits_.max_input_bytes));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString markdown = decoder.decode(utf8_markdown);
    if (decoder.hasError()) {
        return fail(MarkdownPdfErrorCode::InvalidUtf8,
                    QStringLiteral("Markdown input is not canonical UTF-8"));
    }
    if (markdown.trimmed().isEmpty()) {
        return fail(MarkdownPdfErrorCode::EmptyInput,
                    QStringLiteral("Markdown input must contain visible text"));
    }
    if (hasUnsupportedMarkdownControls(markdown)) {
        return fail(MarkdownPdfErrorCode::InvalidUtf8,
                    QStringLiteral("Markdown input contains forbidden control characters"));
    }
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("Markdown PDF rendering requires a QGuiApplication"));
    }

    const auto output_path = absolute_output_path.toString();
    if (const auto output = validateOutputPath(output_path); !output) {
        return std::unexpected(output.error());
    }

    const QPageLayout page_layout(
        QPageSize(QPageSize::Letter), QPageLayout::Portrait,
        QMarginsF(page_margin_points, page_margin_points, page_margin_points, page_margin_points),
        QPageLayout::Point);
    if (!page_layout.isValid()) {
        return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                    QStringLiteral("Cannot construct the fixed US Letter page layout"));
    }
    const auto content_size = page_layout.paintRect(QPageLayout::Point).size();
    auto document_content_size = content_size;
    if (metadata.page_labels) {
        document_content_size.setHeight(document_content_size.height() -
                                        page_label_footer_band_points);
    }
    auto prepared = prepareDocuments(markdown, limits_, document_content_size);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    if (metadata.page_labels) {
        const auto last_number =
            static_cast<qint64>(metadata.page_labels->first_number) + prepared->page_count - 1;
        if (last_number > MarkdownPdfPageLabels::maximum_number) {
            return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                        QStringLiteral("Page label sequence exceeds the maximum label number %1")
                            .arg(MarkdownPdfPageLabels::maximum_number));
        }
    }

    const auto source_sha256 = lowercaseSha256(utf8_markdown);
    const auto title_utf8 = metadata.title.toUtf8();
    const auto title_sha256 = lowercaseSha256(title_utf8);
    const auto provenance = buildProvenance(
        source_sha256, title_sha256, prepared->resolved_body_font,
        prepared->resolved_monospace_font, metadata.page_labels, prepared->page_count);
    const auto provenance_utf8 = provenance.toUtf8();
    const auto semantic_sha256 = semanticRenderDigest(utf8_markdown, title_utf8, provenance_utf8);
    const auto document_id = deterministicDocumentId(semantic_sha256);

    const auto output_information = QFileInfo(output_path);
    QTemporaryFile temporary(
        output_information.dir().filePath(QStringLiteral(".appellate-pdf-XXXXXX.tmp")));
    temporary.setAutoRemove(true);
    if (!temporary.open()) {
        return fail(MarkdownPdfErrorCode::CannotCreateTemporaryOutput,
                    QStringLiteral("Cannot create same-directory temporary PDF output"));
    }
    if (!temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ReadGroup | QFileDevice::ReadOther)) {
        return fail(MarkdownPdfErrorCode::CannotCreateTemporaryOutput,
                    QStringLiteral("Cannot set safe PDF output permissions"));
    }

    BoundedHashingWriteDevice output(temporary, limits_.max_output_bytes);
    if (!output.openForWriting()) {
        return fail(MarkdownPdfErrorCode::CannotCreateTemporaryOutput,
                    QStringLiteral("Cannot open bounded PDF output device"));
    }

    bool render_succeeded = true;
    {
        QPdfWriter writer(&output);
        writer.setResolution(pdf_resolution_dpi);
        writer.setPdfVersion(QPagedPaintDevice::PdfVersion_1_6);
        if (!writer.setPageLayout(page_layout)) {
            return fail(MarkdownPdfErrorCode::InvalidConfiguration,
                        QStringLiteral("QPdfWriter rejected the fixed page layout"));
        }
        writer.setColorModel(QPdfWriter::ColorModel::RGB);
        writer.setTitle(metadata.title);
        writer.setAuthor(QString::fromLatin1(fixed_author));
        writer.setCreator(QString::fromLatin1(fixed_creator));
        writer.setDocumentId(document_id);
        writer.setDocumentXmpMetadata(fixedXmpMetadata(metadata.title, document_id));

        QPainter painter;
        if (!painter.begin(&writer)) {
            render_succeeded = false;
        } else {
            const auto content_rectangle = page_layout.paintRect(QPageLayout::Point);
            const auto full_rectangle = page_layout.fullRect(QPageLayout::Point);
            const auto body_rectangle = metadata.page_labels
                                            ? QRectF(QPointF(0.0, 0.0), document_content_size)
                                            : content_rectangle;
            int emitted_pages{};
            for (const auto& document : prepared->documents) {
                for (int document_page = 0; document_page < document->pageCount();
                     ++document_page) {
                    if (emitted_pages > 0 && !writer.newPage()) {
                        render_succeeded = false;
                        break;
                    }

                    painter.fillRect(full_rectangle, Qt::white);
                    painter.save();
                    painter.setClipRect(body_rectangle);
                    const auto page_offset =
                        static_cast<qreal>(document_page) * body_rectangle.height();
                    painter.translate(body_rectangle.left(), body_rectangle.top() - page_offset);
                    const QRectF document_clip(0.0, page_offset, body_rectangle.width(),
                                               body_rectangle.height());
                    document->drawContents(&painter, document_clip);
                    painter.restore();
                    if (metadata.page_labels) {
                        const auto label_number =
                            static_cast<qint64>(metadata.page_labels->first_number) + emitted_pages;
                        const auto label =
                            metadata.page_labels->prefix + QString::number(label_number);
                        const QRectF label_rectangle(
                            0.0, document_content_size.height() + page_label_top_gap_points,
                            content_size.width(),
                            page_label_footer_band_points - page_label_top_gap_points -
                                page_label_bottom_inset_points);
                        painter.save();
                        painter.setClipRect(label_rectangle);
                        painter.setPen(Qt::black);
                        painter.setFont(configuredFont(body_font_request, QFont::Serif,
                                                       page_label_font_points));
                        painter.drawText(label_rectangle, Qt::AlignHCenter | Qt::AlignVCenter,
                                         label);
                        painter.restore();
                    }
                    ++emitted_pages;
                }
                if (!render_succeeded) {
                    break;
                }
            }
            if (!painter.end()) {
                render_succeeded = false;
            }
            if (emitted_pages != prepared->page_count) {
                render_succeeded = false;
            }
        }
    }
    output.close();

    if (output.exceededLimit()) {
        return fail(MarkdownPdfErrorCode::OutputTooLarge,
                    QStringLiteral("Rendered PDF exceeds the configured %1-byte limit")
                        .arg(limits_.max_output_bytes));
    }
    if (!render_succeeded || output.writeFailed()) {
        return fail(MarkdownPdfErrorCode::CannotRender,
                    QStringLiteral("QPdfWriter could not render the complete document"));
    }
    if (temporary.size() != output.bytesWritten()) {
        return fail(MarkdownPdfErrorCode::CannotRender,
                    QStringLiteral("Rendered PDF byte count is inconsistent"));
    }
    if (!syncFile(temporary)) {
        return fail(MarkdownPdfErrorCode::CannotSyncOutput,
                    QStringLiteral("Cannot durably flush the temporary PDF"));
    }
    temporary.close();

    if (QFileInfo::exists(output_path) || QFileInfo(output_path).isSymLink()) {
        return fail(MarkdownPdfErrorCode::OutputAlreadyExists,
                    QStringLiteral("Refusing to replace a PDF created during rendering"));
    }
    if (!temporary.rename(output_path)) {
        return fail(MarkdownPdfErrorCode::CannotCommitOutput,
                    QStringLiteral("Cannot atomically commit the PDF without overwriting"));
    }
    temporary.setAutoRemove(false);

    return MarkdownPdfResult{source_sha256, semantic_sha256,      output.sha256(),
                             provenance,    prepared->page_count, output.bytesWritten()};
}

} // namespace appellate::content
