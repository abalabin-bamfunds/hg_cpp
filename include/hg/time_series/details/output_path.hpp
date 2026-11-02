#pragma once

#include <hg/evaluation_time.hpp>
#include <hg/time_series/item_id.hpp>
#include <hg/time_series/time_series_state.hpp>

#include <cstddef>

namespace hg {

class EvaluationContext;
class TimeSeriesRuntimeType;

namespace details {

class OutputPath;

[[nodiscard]] void *add_output_path(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type,
                                    TimeSeriesState &last_updated, void *value, item_id_t id, void *parent);
EvaluationContext &output_path_context(void *path) noexcept;
void publish_output_path(void *path, bool item_was_valid) noexcept;
void release_output_path(void *path) noexcept;

} // namespace details

} // namespace hg
