#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>

namespace bg::details {

struct SampledSet;

SampledSet *make_set_sample(ResolvedTimeSeries previous, ResolvedTimeSeries current, EvaluationTime evaluation_time,
                            allocator &memory);
void destroy_set_sample(SampledSet *sample) noexcept;
TimeSeriesRuntimeType::ConstItem set_sample_item(const SampledSet &sample) noexcept;
ResolvedTimeSeries set_sample_previous(const SampledSet &sample) noexcept;

} // namespace bg::details
