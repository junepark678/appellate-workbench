#pragma once

#include "appellate/model/procedure.hpp"

namespace appellate::engine {

[[nodiscard]] bool isBusinessDay(const model::CourtCalendar& calendar,
                                 model::LegalDate date) noexcept;

[[nodiscard]] model::LegalDate calculateDeadline(const model::CourtCalendar& calendar,
                                                 model::LegalDate start,
                                                 const model::CureDeadlineRule& rule) noexcept;

} // namespace appellate::engine
