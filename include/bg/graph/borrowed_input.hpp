#pragma once

#include <bg/details/evaluation_context.hpp>
#include <bg/time_series/time_series_state.hpp>

#include <cstddef>
#include <span>

namespace bg {

class RuntimeGraph;
class TimeSeriesRuntimeType;

enum class BorrowedInputChange : unsigned char {
    unchanged,
    update,
    invalidation,
    revalidation,
};

struct BorrowedInput {
    const TimeSeriesState *last_updated;
    const void *value;
    const TimeSeriesRuntimeType *runtime_type;
    BorrowedInputChange change;
};

namespace details {

struct BorrowedInputSection : EvaluationContextSection {
    BorrowedInputSection(const RuntimeGraph &graph, std::span<const BorrowedInput> inputs) noexcept
        : EvaluationContextSection{nullptr, &graph}, inputs{inputs} {}

    std::span<const BorrowedInput> inputs;
};

} // namespace details

} // namespace bg
