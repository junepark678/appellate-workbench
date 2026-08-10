#pragma once

#include "appellate/engine/error.hpp"
#include "appellate/model/oral_argument.hpp"

#include <expected>
#include <span>
#include <string>

namespace appellate::engine {

[[nodiscard]] auto behaviorDefinitionDigest(const model::BenchConfiguration& bench)
    -> std::expected<std::string, Error>;

[[nodiscard]] auto groundingDigest(const model::ArgumentGrounding& grounding)
    -> std::expected<std::string, Error>;

[[nodiscard]] auto initializeOralArgument(const model::OralArgumentConfiguration& configuration,
                                          const model::BenchConfiguration& bench,
                                          const model::ArgumentGrounding& grounding)
    -> std::expected<model::OralArgumentState, Error>;

[[nodiscard]] auto planOpeningQuestion(const model::OralArgumentConfiguration& configuration,
                                       const model::BenchConfiguration& bench,
                                       const model::ArgumentGrounding& grounding,
                                       const model::OralArgumentState& state)
    -> std::expected<model::OralArgumentEvent, Error>;

[[nodiscard]] auto decideCounselAnswer(const model::OralArgumentConfiguration& configuration,
                                       const model::BenchConfiguration& bench,
                                       const model::ArgumentGrounding& grounding,
                                       const model::OralArgumentState& state,
                                       const model::CounselAnswer& answer)
    -> std::expected<model::OralArgumentEvent, Error>;

[[nodiscard]] auto applyOralArgumentEvent(const model::OralArgumentConfiguration& configuration,
                                          const model::BenchConfiguration& bench,
                                          const model::ArgumentGrounding& grounding,
                                          const model::OralArgumentState& state,
                                          const model::OralArgumentEvent& event)
    -> std::expected<model::OralArgumentState, Error>;

[[nodiscard]] auto replayOralArgument(const model::OralArgumentConfiguration& configuration,
                                      const model::BenchConfiguration& bench,
                                      const model::ArgumentGrounding& grounding,
                                      const model::OralArgumentState& initial_state,
                                      std::span<const model::OralArgumentEvent> events)
    -> std::expected<model::OralArgumentState, Error>;

[[nodiscard]] bool isQuestionAct(model::BenchActKind kind) noexcept;

} // namespace appellate::engine
