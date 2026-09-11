#pragma once

#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>

namespace bg::details {

ReferenceRepresentation *make_tsd_item_reference_representation(
    ReferenceProvider &provider, Reference base_reference, const TimeSeriesDictionaryType &type,
    const TimeSeriesDictionaryRuntimeType &base_runtime_type, const TimeSeriesRuntimeType &item_runtime_type,
    std::pmr::memory_resource &memory);

ReferenceRepresentation *make_dereferenced_tsd_representation(ReferenceProvider &provider,
                                                              Reference base_reference,
                                                              const TimeSeriesDictionaryType &type,
                                                              const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                              const TimeSeriesRuntimeType &item_runtime_type,
                                                              std::pmr::memory_resource &memory);

} // namespace bg::details
