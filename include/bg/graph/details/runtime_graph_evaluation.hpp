#pragma once

#include <bg/details/evaluation_context.hpp>

#include <cstddef>
#include <limits>
#include <optional>

namespace bg::details {

// Retains the node at which a runtime graph paused evaluation.
struct RuntimeGraphEvaluation {
    static constexpr auto not_paused = std::numeric_limits<std::size_t>::max();

    bool paused() const noexcept { return node_index != not_paused; }

    void reset() noexcept { node_index = not_paused; }

    std::size_t node_index{not_paused};
};

static_assert(sizeof(RuntimeGraphEvaluation) == sizeof(void *));

using EvaluationResult = std::optional<PendingEvaluation>;

} // namespace bg::details
