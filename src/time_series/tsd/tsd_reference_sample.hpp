#pragma once

#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>


namespace bg::details {

struct SampledDictionary;

SampledDictionary *make_dictionary_sample(ResolvedTimeSeries previous, ResolvedTimeSeries current,
                                          EvaluationTime evaluation_time, allocator &memory);
void destroy_dictionary_sample(SampledDictionary *sample) noexcept;
TimeSeriesRuntimeType::ConstItem dictionary_sample_item(const SampledDictionary &sample) noexcept;
ResolvedTimeSeries dictionary_sample_previous(const SampledDictionary &sample) noexcept;

} // namespace bg::details
