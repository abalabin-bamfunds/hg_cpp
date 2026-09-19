#pragma once

#include <bg/graph/node.hpp>
#include <bg/time_series/tsl/tsl_free_storage.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>

namespace bg::details {

Node free_list_node(const free_fixed_time_series_list_storage &storage) noexcept;
bool free_list_valid(const free_fixed_time_series_list_storage &storage) noexcept;

class FreeFixedTimeSeriesListRuntimeType final : public TimeSeriesRuntimeType {
  public:
    explicit FreeFixedTimeSeriesListRuntimeType(const TimeSeriesListType &type);

    bool collection_valid(const void *value) const noexcept override;
    std::size_t item_count(const void *value) const override;
    ConstItem item(const void *value, item_id_t id) const override;
    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override;
    bool item_updated(const EvaluationTime &last_updated, const void *value,
                      item_id_t id) const noexcept override;
    void for_each_updated_item(const EvaluationTime &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override;
    void for_each_modified_item(const EvaluationTime &last_updated, const void *value,
                                EvaluationTime evaluation_time, void *context,
                                ModifiedItemFunction function) const override;
    void construct_initial_value(void *destination) const noexcept override;
    void destroy_value(void *value, allocator &memory) const noexcept override;
};

const TimeSeriesRuntimeType &free_fixed_time_series_list_runtime_type(const TimeSeriesListType &type);

} // namespace bg::details
