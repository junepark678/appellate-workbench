#pragma once

#include "appellate/content/markdown_pdf_renderer.hpp"

#include <QByteArray>
#include <QStringList>

namespace appellate::content {

enum class RenderCliExitCode : int {
    Success = 0,
    InvalidArguments = 2,
    InvalidPlan = 3,
    RenderFailed = 4,
    OperationFailed = 5,
};

struct RenderCliResult final {
    int exit_code{};
    QByteArray standard_output;
    QByteArray standard_error;

    friend bool operator==(const RenderCliResult&, const RenderCliResult&) = default;
};

struct RenderBatchLimits final {
    static constexpr qint64 default_maximum_plan_bytes = 2LL * 1024LL * 1024LL;
    static constexpr qsizetype default_maximum_entries = 512;
    static constexpr qsizetype default_maximum_segments_per_entry = 512;
    static constexpr qsizetype default_maximum_path_bytes = 512;
    static constexpr qsizetype default_maximum_title_bytes = 512;
    static constexpr qsizetype default_maximum_inline_markdown_bytes = 256 * 1024;
    static constexpr qint64 default_maximum_total_string_bytes = 1024LL * 1024LL;
    static constexpr qint64 default_maximum_total_source_bytes = 256LL * 1024LL * 1024LL;
    static constexpr qint64 default_maximum_total_assembled_bytes = 256LL * 1024LL * 1024LL;
    static constexpr qint64 default_maximum_total_output_bytes = 1024LL * 1024LL * 1024LL;

    qint64 maximum_plan_bytes{default_maximum_plan_bytes};
    qsizetype maximum_entries{default_maximum_entries};
    qsizetype maximum_segments_per_entry{default_maximum_segments_per_entry};
    qsizetype maximum_path_bytes{default_maximum_path_bytes};
    qsizetype maximum_title_bytes{default_maximum_title_bytes};
    qsizetype maximum_inline_markdown_bytes{default_maximum_inline_markdown_bytes};
    qint64 maximum_total_string_bytes{default_maximum_total_string_bytes};
    qint64 maximum_total_source_bytes{default_maximum_total_source_bytes};
    qint64 maximum_total_assembled_bytes{default_maximum_total_assembled_bytes};
    qint64 maximum_total_output_bytes{default_maximum_total_output_bytes};
    MarkdownPdfLimits document_limits{};

    friend bool operator==(const RenderBatchLimits&, const RenderBatchLimits&) = default;
};

// Arguments exclude the executable name:
//   <absolute-plan.json> <absolute-source-root> <new-absolute-output-directory>
//
// A schema-v1 plan has exactly {schema_version, entries}. Each entry has exactly
// {source_path, output_path, title}, or {segments, output_path, title} with the optional
// front_matter_markdown field. A segment has exactly {source_path, first_page, last_page}; pages
// are one-based Markdown segments delimited by MarkdownPdfRenderer::pageBreakMarker().
//
// On success the output directory contains the rendered PDFs and inventory.json. The directory is
// published by a same-filesystem rename only after every PDF and the inventory are complete.
[[nodiscard]] RenderCliResult runRenderCli(const QStringList& arguments,
                                           const RenderBatchLimits& limits = {});

} // namespace appellate::content
