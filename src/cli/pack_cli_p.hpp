#pragma once

#include "independent_review_publisher_p.hpp"
#include "pack_cli.hpp"

#include <QDate>

#include <functional>

namespace appellate::cli::detail {

using CurrentUtcDateProvider = std::function<QDate()>;

struct IndependentReviewCliHooks final {
    std::function<void(QByteArray& manifest_bytes, QByteArray& review_bytes)>
        replace_finalized_publication_members;
    IndependentReviewPublisherHooks publisher;
};

[[nodiscard]] RunResult runPackCli(const QStringList& arguments,
                                   const CurrentUtcDateProvider& current_utc_date_provider,
                                   const IndependentReviewCliHooks& hooks = {});

} // namespace appellate::cli::detail
