#pragma once

#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

namespace bg::details {

class TimeSeriesDictionaryRepresentation : public ReferenceRepresentation {
  public:
    TimeSeriesDictionaryRepresentation(Reference base_reference, const TimeSeriesDictionaryType &type);

    const TimeSeriesDictionaryRuntimeType &dictionary_runtime_type() const noexcept { return *runtime_type_; }

    virtual ResolvedTimeSeries base_time_series() const = 0;
    virtual TimeSeriesRuntimeType::ConstItem item(item_id_t id) const = 0;
    virtual bool item_updated(const TimeSeriesState &last_updated, item_id_t id) const noexcept = 0;
    virtual bool is_updated_item(const TimeSeriesState &last_updated, item_id_t id) const noexcept = 0;
    virtual bool is_modified_item(const TimeSeriesState &last_updated, item_id_t id) const noexcept = 0;
    virtual item_id_t next_updated_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept = 0;
    virtual item_id_t next_modified_item_id(const TimeSeriesState &last_updated, item_id_t start) const noexcept = 0;
    virtual void for_each_updated(const TimeSeriesState &last_updated, void *context,
                                  TimeSeriesRuntimeType::ItemIdFunction function) const = 0;
    virtual void for_each_modified(const TimeSeriesState &last_updated, void *context,
                                   TimeSeriesRuntimeType::ItemIdFunction function) const = 0;
    virtual void for_each_updated_item(const TimeSeriesState &last_updated, void *context,
                                       TimeSeriesRuntimeType::ItemIdFunction function) const = 0;
    virtual void for_each_modified_item(const TimeSeriesState &last_updated, EvaluationTime evaluation_time,
                                        void *context, TimeSeriesRuntimeType::ModifiedItemFunction function) const = 0;

  private:
    const TimeSeriesDictionaryRuntimeType *runtime_type_;
};

const TimeSeriesDictionaryRuntimeType &time_series_dictionary_representation_runtime_type(
    const TimeSeriesDictionaryType &type);

ReferenceRepresentation *make_tsd_item_reference_representation(
    ReferenceProvider &provider, Reference base_reference, const TimeSeriesDictionaryType &type,
    const TimeSeriesDictionaryRuntimeType &base_runtime_type, const TimeSeriesRuntimeType &item_runtime_type,
    allocator &memory);

ReferenceRepresentation *make_dereferenced_tsd_representation(ReferenceProvider &provider,
                                                              Reference base_reference,
                                                              const TimeSeriesDictionaryType &type,
                                                              const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                              const TimeSeriesRuntimeType &item_runtime_type,
                                                              allocator &memory);

ReferenceRepresentation *make_recursive_tsd_representation(ReferenceProvider &provider, Reference base_reference,
                                                           const TimeSeriesDictionaryType &type,
                                                           const TimeSeriesDictionaryRuntimeType &base_runtime_type,
                                                           const TimeSeriesRuntimeType &item_runtime_type,
                                                           allocator &memory);

} // namespace bg::details
