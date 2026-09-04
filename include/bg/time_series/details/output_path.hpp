#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/time_series/item_id.hpp>

#include <cstddef>

namespace bg {

class EvaluationContext;
class TimeSeriesRuntimeType;

namespace details {

[[nodiscard]] void *add_output_path(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type,
                                    EvaluationTime &last_updated, void *value, TimeSeriesItemId id, void *parent);
[[nodiscard]] EvaluationContext &output_path_context(void *path) noexcept;
void publish_output_path(void *path) noexcept;
void release_output_path(void *path) noexcept;

} // namespace details

} // namespace bg
