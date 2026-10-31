#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tss/tss_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../../utils/counting_memory_resource.hpp"

#include <cstdint>

using namespace bg;

TEST_CASE("canceling a time series set addition reclaims its position immediately",
          "[value][tss][series:tss][storage][feature:time-series-storage][transition:addition]") {
    auto resource = counting_memory_resource{};
    auto memory = allocator{resource};
    auto storage = details::time_series_set_storage{};
    const auto &key_runtime_type = cpp_scalar_runtime_type<i64>();
    const auto key_layout = util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto key = ScalarValue<i64>{1};
    auto constructed_items = 0;

    storage.reset_change_tracking(MIN_DT + EvaluationTime::duration{1}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto inserted = storage.insert(
        key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(inserted.change == details::time_series_set_change::added);

    const auto erased =
        storage.erase(key, key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });

    CHECK(erased.change == details::time_series_set_change::canceled);
    CHECK(constructed_items == 0);
    CHECK(storage.size() == 0);
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == util::occupancy_index::npos);
    CHECK_FALSE(storage.changed());

    const auto inserted_again = storage.insert(
        key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(inserted_again.position == inserted.position);

    storage.release(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);
    CHECK(resource.allocations() == 0);
}

TEST_CASE("removing an existing time series set item retains it until reclamation",
          "[value][tss][series:tss][storage][feature:time-series-storage][transition:removal]") {
    auto resource = counting_memory_resource{};
    auto memory = allocator{resource};
    auto storage = details::time_series_set_storage{};
    const auto &key_runtime_type = cpp_scalar_runtime_type<i64>();
    const auto key_layout = util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto key = ScalarValue<i64>{1};
    auto constructed_items = 0;
    const auto first_time = MIN_DT + EvaluationTime::duration{1};

    storage.reset_change_tracking(first_time, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto inserted = storage.insert(
        key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    storage.reset_change_tracking(first_time + EvaluationTime::duration{1}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });

    const auto removed =
        storage.erase(key, key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(removed.change == details::time_series_set_change::removed);
    CHECK(constructed_items == 1);
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == inserted.position);

    const auto restored = storage.insert(
        key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(restored.change == details::time_series_set_change::restored);
    CHECK(restored.position == inserted.position);
    CHECK_FALSE(storage.changed());

    const auto removed_again =
        storage.erase(key, key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(removed_again.change == details::time_series_set_change::removed);
    storage.reset_change_tracking(first_time + EvaluationTime::duration{2}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == util::occupancy_index::npos);

    storage.release(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(resource.allocations() == 0);
}

TEST_CASE("retaining a time series set key preserves its non-live position",
          "[value][tss][series:tss][storage][feature:time-series-storage][feature:getitem]"
          "[transition:addition][transition:removal]") {
    auto resource = counting_memory_resource{};
    auto memory = allocator{resource};
    auto storage = details::time_series_set_storage{};
    const auto &key_runtime_type = cpp_scalar_runtime_type<i64>();
    const auto key_layout = util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto key = ScalarValue<i64>{1};
    const auto first_time = MIN_DT + EvaluationTime::duration{1};
    auto constructed_items = 0;

    storage.reset_change_tracking(first_time, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto retained = storage.retain(key, key_runtime_type, key_layout, memory);
    const auto retained_again = storage.retain(key, key_runtime_type, key_layout, memory);

    CHECK(retained_again == retained);
    CHECK(storage.size() == 0);
    CHECK_FALSE(storage.changed());

    storage.release_retention(retained, key_runtime_type, key_layout, memory);
    storage.reset_change_tracking(first_time + EvaluationTime::duration{1}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == retained);

    const auto inserted = storage.insert(
        key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(inserted.change == details::time_series_set_change::added);
    CHECK(inserted.position == retained);

    storage.reset_change_tracking(first_time + EvaluationTime::duration{2}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto removed =
        storage.erase(key, key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(removed.change == details::time_series_set_change::removed);
    storage.reset_change_tracking(first_time + EvaluationTime::duration{3}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });

    CHECK(constructed_items == 0);
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == retained);

    storage.release_retention(retained, key_runtime_type, key_layout, memory);
    storage.reset_change_tracking(first_time + EvaluationTime::duration{4}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(storage.find_position(key, key_runtime_type, key_layout) == util::occupancy_index::npos);

    storage.release(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(resource.allocations() == 0);
}

TEST_CASE("clearing time series set storage retains only previously visible items",
          "[value][tss][series:tss][storage][feature:time-series-storage][transition:removal]") {
    auto resource = counting_memory_resource{};
    auto memory = allocator{resource};
    auto storage = details::time_series_set_storage{};
    const auto &key_runtime_type = cpp_scalar_runtime_type<i64>();
    const auto key_layout = util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto existing_key = ScalarValue<i64>{1};
    const auto new_key = ScalarValue<i64>{2};
    auto constructed_items = 0;
    const auto first_time = MIN_DT + EvaluationTime::duration{1};

    storage.reset_change_tracking(first_time, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto existing = storage.insert(
        existing_key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    storage.reset_change_tracking(first_time + EvaluationTime::duration{1}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto added = storage.insert(
        new_key, key_runtime_type, key_layout, memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });

    storage.clear(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });

    CHECK(storage.size() == 0);
    CHECK(storage.removed().contains(existing.position));
    CHECK(storage.find_position(existing_key, key_runtime_type, key_layout) == existing.position);
    CHECK(storage.find_position(new_key, key_runtime_type, key_layout) == util::occupancy_index::npos);
    CHECK_FALSE(storage.removed().contains(added.position));
    CHECK(constructed_items == 1);

    storage.reset_change_tracking(first_time + EvaluationTime::duration{2}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);

    storage.release(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(resource.allocations() == 0);
}
