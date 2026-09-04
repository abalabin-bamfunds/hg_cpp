#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <vector>

TEST_CASE("dictionary runtime items use stable sparse IDs",
          "[value][time_series][tsd][series:tsd][runtime_type][feature:time-series-storage]") {
    using Dictionary = bg::TSD<std::int64_t, bg::TS<int>>;

    const auto &runtime_type = bg::cpp_time_series_runtime_type<Dictionary>();
    const auto &dictionary_runtime_type =
        static_cast<const bg::details::TimeSeriesDictionaryRuntimeType &>(runtime_type);
    const auto &key_runtime_type = dictionary_runtime_type.key_runtime_type();
    const auto &item_runtime_type = dictionary_runtime_type.item_runtime_type();
    const auto key_layout = bg::util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto item_layout = bg::details::contained_time_series_layout(item_runtime_type);
    const auto item_value_offset = bg::details::contained_time_series_value_offset(item_runtime_type);

    auto memory = std::pmr::monotonic_buffer_resource{};
    auto *value = memory.allocate(runtime_type.value_size(), runtime_type.value_alignment());
    runtime_type.construct_runtime_value(value, memory);
    auto &storage = *static_cast<bg::details::time_series_dict_storage *>(value);

    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    storage.reset_change_tracking(update_time, key_runtime_type, key_layout, item_runtime_type, item_layout,
                                  item_value_offset, memory);
    const auto key = bg::ScalarValue<std::int64_t>{7};
    const auto inserted = storage.item(key.input_view(), key_runtime_type, key_layout, item_runtime_type, item_layout,
                                       item_value_offset, memory);
    storage.key_storage().last_updated = update_time;

    auto item = runtime_type.item(value, inserted.position);
    item.last_updated = update_time;
    *static_cast<int *>(item.value) = 42;
    REQUIRE(storage.mark_item_modified(inserted.position, update_time, memory));

    auto ids = std::vector<bg::TimeSeriesItemId>{};
    runtime_type.for_each_item_id(value, &ids, [](void *context, bg::TimeSeriesItemId id) {
        static_cast<std::vector<bg::TimeSeriesItemId> *>(context)->push_back(id);
    });
    CHECK(runtime_type.item_count(value) == 1);
    CHECK(ids == std::vector<bg::TimeSeriesItemId>{inserted.position});

    const auto const_item = runtime_type.item(static_cast<const void *>(value), inserted.position);
    CHECK(&const_item.runtime_type == &item_runtime_type);
    CHECK(const_item.last_updated == update_time);
    CHECK(*static_cast<const int *>(const_item.value) == 42);

    const auto key_set = runtime_type.item(static_cast<const void *>(value), bg::key_set_item_id);
    CHECK(key_set.last_updated == update_time);
    CHECK(key_set.value == &storage.key_storage().value);

    auto updated = std::vector<bg::TimeSeriesItemId>{};
    runtime_type.for_each_updated_item(update_time, value, &updated, [](void *context, bg::TimeSeriesItemId id) {
        static_cast<std::vector<bg::TimeSeriesItemId> *>(context)->push_back(id);
    });
    CHECK(updated == std::vector<bg::TimeSeriesItemId>{bg::key_set_item_id, inserted.position});

    runtime_type.destroy_value(value, memory);
}
