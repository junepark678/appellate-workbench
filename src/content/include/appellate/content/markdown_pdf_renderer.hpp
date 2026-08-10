#pragma once

#include <QByteArrayView>
#include <QString>
#include <QStringView>

#include <expected>
#include <optional>
#include <utility>

namespace appellate::content {

enum class MarkdownPdfErrorCode {
    InvalidConfiguration,
    InvalidUtf8,
    EmptyInput,
    InputTooLarge,
    UnsafeSourcePath,
    SourceNotFound,
    SourceNotRegularFile,
    CannotReadSource,
    UnsafeOutputPath,
    OutputAlreadyExists,
    UnsupportedContent,
    RequiredFontUnavailable,
    PageLimitExceeded,
    CannotCreateTemporaryOutput,
    CannotRender,
    OutputTooLarge,
    CannotSyncOutput,
    CannotCommitOutput,
};

struct MarkdownPdfError final {
    MarkdownPdfErrorCode code;
    QString message;

    friend bool operator==(const MarkdownPdfError&, const MarkdownPdfError&) = default;
};

struct MarkdownPdfLimits final {
    static constexpr qint64 default_max_input_bytes = 2LL * 1024LL * 1024LL;
    static constexpr qint64 default_max_output_bytes = 64LL * 1024LL * 1024LL;
    static constexpr int default_max_pages = 512;

    qint64 max_input_bytes{default_max_input_bytes};
    qint64 max_output_bytes{default_max_output_bytes};
    int max_pages{default_max_pages};

    friend bool operator==(const MarkdownPdfLimits&, const MarkdownPdfLimits&) = default;
};

struct MarkdownPdfPageLabels final {
    static constexpr qsizetype maximum_prefix_bytes = 16;
    static constexpr int maximum_number = 999'999'999;

    // Prefixes are deliberately restricted to 1-16 uppercase ASCII letters. The rendered label
    // is the prefix immediately followed by the base-10 page number, for example JA1.
    QString prefix;
    int first_number{1};

    friend bool operator==(const MarkdownPdfPageLabels&, const MarkdownPdfPageLabels&) = default;
};

struct MarkdownPdfMetadata final {
    QString title{QStringLiteral("Synthetic Appellate Record")};
    std::optional<MarkdownPdfPageLabels> page_labels;

    MarkdownPdfMetadata() = default;
    explicit MarkdownPdfMetadata(
        QString document_title,
        std::optional<MarkdownPdfPageLabels> document_page_labels = std::nullopt)
        : title(std::move(document_title)), page_labels(std::move(document_page_labels)) {}

    friend bool operator==(const MarkdownPdfMetadata&, const MarkdownPdfMetadata&) = default;
};

struct MarkdownPdfResult final {
    // Hash of the exact canonical UTF-8 Markdown bytes supplied by the caller.
    QString source_sha256;

    // Hash of the source, metadata, renderer contract, Qt environment, and layout settings.
    // It identifies a semantic/layout render; it is not a hash of the PDF bytes.
    QString semantic_render_sha256;

    // Hash of the exact PDF bytes committed at output_path. Pack manifests use this value.
    QString pdf_sha256;

    QString renderer_provenance;
    int page_count{};
    qint64 output_bytes{};

    friend bool operator==(const MarkdownPdfResult&, const MarkdownPdfResult&) = default;
};

class MarkdownPdfRenderer final {
  public:
    explicit MarkdownPdfRenderer(MarkdownPdfLimits limits = {});

    [[nodiscard]] auto render(QByteArrayView utf8_markdown, QStringView absolute_output_path,
                              const MarkdownPdfMetadata& metadata = {}) const
        -> std::expected<MarkdownPdfResult, MarkdownPdfError>;

    [[nodiscard]] auto renderFile(QStringView absolute_markdown_path,
                                  QStringView absolute_output_path,
                                  const MarkdownPdfMetadata& metadata = {}) const
        -> std::expected<MarkdownPdfResult, MarkdownPdfError>;

    [[nodiscard]] const MarkdownPdfLimits& limits() const noexcept;

    [[nodiscard]] static QStringView pageBreakMarker() noexcept;
    [[nodiscard]] static QStringView rendererContractVersion() noexcept;

    // QPdfWriter writes wall-clock creation/modification timestamps. Consequently, exact PDF
    // bytes are deliberately not a reproducibility promise; pdf_sha256 always identifies the
    // bytes actually emitted. The semantic render digest is the repeatable authoring identity.
    [[nodiscard]] static constexpr bool byteOutputIsDeterministic() noexcept { return false; }

  private:
    MarkdownPdfLimits limits_;
};

} // namespace appellate::content
