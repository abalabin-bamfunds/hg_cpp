#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory_resource>
#include <stdexcept>
#include <vector>

TEST_CASE("fixed list changes cross bitset word boundaries",
          "[value][time_series][tsl][series:fixed-tsl][feature:time-series-storage][transition:update]") {
    auto changes = bg::details::fixed_time_series_list_changes<65>{};
    const auto first_time = bg::MIN_DT + std::chrono::microseconds{1};

    CHECK(changes.mark(0, first_time));
    CHECK(changes.mark(63, first_time));
    CHECK(changes.mark(64, first_time));
    CHECK_FALSE(changes.mark(64, first_time));
    CHECK(std::vector<std::size_t>{changes.begin(), changes.end()} == std::vector<std::size_t>{0, 63, 64});

    const auto second_time = first_time + std::chrono::microseconds{1};
    CHECK(changes.mark(1, second_time));
    CHECK(std::vector<std::size_t>{changes.begin(), changes.end()} == std::vector<std::size_t>{1});
}

TEST_CASE("fixed list validity is derived from its items",
          "[value][time_series][tsl][series:fixed-tsl][feature:time-series-storage]") {
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 3>>();
    auto memory = std::pmr::monotonic_buffer_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(runtime_type.value_size(), runtime_type.value_alignment());
    runtime_type.construct_runtime_value(storage, allocator);

    CHECK_FALSE(runtime_type.collection_valid(storage));

    const auto item = runtime_type.item_layout(1);
    auto &last_updated =
        *reinterpret_cast<bg::TimeSeriesState *>(static_cast<std::byte *>(storage) + item.last_updated_offset);
    last_updated.publish(bg::MIN_DT + std::chrono::microseconds{1});
    CHECK(runtime_type.collection_valid(storage));

    last_updated.invalidate(bg::MIN_DT + std::chrono::microseconds{2});
    CHECK_FALSE(runtime_type.collection_valid(storage));

    runtime_type.destroy_value(storage, allocator);
}

TEST_CASE("fixed list runtime items use positional IDs",
          "[value][time_series][tsl][series:fixed-tsl][runtime_type][feature:time-series-storage]") {
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 3>>();
    auto memory = std::pmr::monotonic_buffer_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(runtime_type.value_size(), runtime_type.value_alignment());
    runtime_type.construct_runtime_value(storage, allocator);

    auto ids = std::vector<bg::item_id_t>{};
    runtime_type.for_each_item_id(storage, &ids, [](void *context, bg::item_id_t id) {
        static_cast<std::vector<bg::item_id_t> *>(context)->push_back(id);
    });
    CHECK(runtime_type.item_count(storage) == 3);
    CHECK(ids == std::vector<bg::item_id_t>{0, 1, 2});

    const auto update_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto item = runtime_type.item(storage, 1);
    item.last_updated.publish(update_time);
    *static_cast<int *>(item.value) = 42;

    const auto const_item = runtime_type.item(static_cast<const void *>(storage), 1);
    CHECK(&const_item.runtime_type == &bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>());
    CHECK(const_item.last_updated.time() == update_time);
    CHECK(*static_cast<const int *>(const_item.value) == 42);

    runtime_type.destroy_value(storage, allocator);
}

TEST_CASE("fixed list storage reports item cleanup requirements",
          "[value][time_series][tsl][series:fixed-tsl][storage][feature:time-series-storage]") {
    const auto &storage = bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 2>>();

    CHECK_FALSE(storage.uses_variable_memory());
    CHECK_FALSE(storage.requires_finalization());
}

TEST_CASE("variable-size list storage is not implemented",
          "[value][time_series][tsl][series:fixed-tsl][storage][feature:time-series-storage]"
          "[storage:variable]") {
    const auto &item = bg::TS<bg::i64>::type();
    const auto &list = bg::time_series_list_type(item, std::dynamic_extent, bg::TimeSeriesGuarantee::ordinary);

    CHECK_THROWS_AS(bg::resolve_time_series_runtime_type(list), std::invalid_argument);
}
