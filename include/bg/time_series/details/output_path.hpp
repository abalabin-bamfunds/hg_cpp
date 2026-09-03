#pragma once

#include <bg/evaluation_time.hpp>

#include <cstddef>

namespace bg {

class EvaluationContext;
class TimeSeriesStorageRepresentation;

namespace details {

[[nodiscard]] void *add_output_path(EvaluationContext &context,
                                    const TimeSeriesStorageRepresentation &representation,
                                    EvaluationTime &last_updated, void *value, std::size_t item, void *parent);
[[nodiscard]] EvaluationContext &output_path_context(void *path) noexcept;
void publish_output_path(void *path) noexcept;
void release_output_path(void *path) noexcept;

} // namespace details

} // namespace bg
