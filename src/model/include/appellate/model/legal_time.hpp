#pragma once

#include <chrono>

namespace appellate::model {

struct LegalDate final {
    std::chrono::year_month_day value;

    friend bool operator==(const LegalDate&, const LegalDate&) = default;
};

// Both values are recorded inputs. The kernel never consults a wall clock or a time-zone
// database. `court_date` is the authoritative local date for deadline computation.
struct LegalTime final {
    std::chrono::sys_seconds instant;
    LegalDate court_date;

    friend bool operator==(const LegalTime&, const LegalTime&) = default;
};

} // namespace appellate::model
