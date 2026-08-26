#include "appellate/engine/business_calendar.hpp"

#include <algorithm>
#include <chrono>

namespace appellate::engine {
namespace {

[[nodiscard]] auto asDays(model::LegalDate date) noexcept -> std::chrono::sys_days {
    return std::chrono::sys_days{date.value};
}

[[nodiscard]] auto fromDays(std::chrono::sys_days date) noexcept -> model::LegalDate {
    return model::LegalDate{std::chrono::year_month_day{date}};
}

} // namespace

bool isBusinessDay(const model::CourtCalendar& calendar, model::LegalDate date) noexcept {
    const auto day = asDays(date);
    const auto weekday = std::chrono::weekday{day};
    if (weekday == std::chrono::Saturday || weekday == std::chrono::Sunday) {
        return false;
    }

    return std::ranges::none_of(calendar.holidays,
                                [day](model::LegalDate holiday) { return asDays(holiday) == day; });
}

model::LegalDate calculateDeadline(const model::CourtCalendar& calendar, model::LegalDate start,
                                   const model::CureDeadlineRule& rule) noexcept {
    auto cursor = asDays(start);
    auto remaining = rule.days;

    while (remaining > 0) {
        cursor += std::chrono::days{1};
        if (rule.counting == model::DeadlineCounting::CalendarDays ||
            isBusinessDay(calendar, fromDays(cursor))) {
            --remaining;
        }
    }

    if (rule.roll_forward_to_business_day) {
        while (!isBusinessDay(calendar, fromDays(cursor))) {
            cursor += std::chrono::days{1};
        }
    }

    return fromDays(cursor);
}

} // namespace appellate::engine
