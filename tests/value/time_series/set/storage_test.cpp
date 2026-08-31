#include <bg/scalar/value.hpp>
#include <bg/time_series/tss/tss_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../../utils/counting_memory_resource.hpp"

#include <cstdint>

TEST_CASE("canceling a time series set addition reclaims its position immediately", "[value][tss][storage]") {
    auto memory = counting_memory_resource{};
    auto storage = bg::details::time_series_set_storage<std::int64_t>{};
    const auto key = bg::ScalarValue<std::int64_t>{1};
    auto constructed_items = 0;

    storage.reset_change_tracking(bg::MIN_DT + bg::EvaluationTime::duration{1}, bg::MIN_DT, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto inserted = storage.insert(
        key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(inserted.change == bg::details::time_series_set_change::added);

    const auto erased =
        storage.erase(key.input_view(), memory, [&](std::uint32_t) noexcept { --constructed_items; });

    CHECK(erased.change == bg::details::time_series_set_change::canceled);
    CHECK(constructed_items == 0);
    CHECK(storage.size() == 0);
    CHECK(storage.find_position(key.input_view()) == bg::util::occupancy_index::npos);
    CHECK_FALSE(storage.changed());

    const auto inserted_again = storage.insert(
        key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(inserted_again.position == inserted.position);

    storage.release(memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("removing an existing time series set item retains it until reclamation", "[value][tss][storage]") {
    auto memory = counting_memory_resource{};
    auto storage = bg::details::time_series_set_storage<std::int64_t>{};
    const auto key = bg::ScalarValue<std::int64_t>{1};
    auto constructed_items = 0;
    const auto first_time = bg::MIN_DT + bg::EvaluationTime::duration{1};

    storage.reset_change_tracking(first_time, bg::MIN_DT, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto inserted = storage.insert(
        key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    storage.reset_change_tracking(first_time + bg::EvaluationTime::duration{1}, first_time, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });

    const auto removed =
        storage.erase(key.input_view(), memory, [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(removed.change == bg::details::time_series_set_change::removed);
    CHECK(constructed_items == 1);
    CHECK(storage.find_position(key.input_view()) == inserted.position);

    const auto restored = storage.insert(
        key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(restored.change == bg::details::time_series_set_change::restored);
    CHECK(restored.position == inserted.position);
    CHECK_FALSE(storage.changed());

    const auto removed_again =
        storage.erase(key.input_view(), memory, [&](std::uint32_t) noexcept { --constructed_items; });
    REQUIRE(removed_again.change == bg::details::time_series_set_change::removed);
    storage.reset_change_tracking(first_time + bg::EvaluationTime::duration{2}, first_time, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);
    CHECK(storage.find_position(key.input_view()) == bg::util::occupancy_index::npos);

    storage.release(memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(memory.allocations() == 0);
}

TEST_CASE("clearing time series set storage retains only previously visible items", "[value][tss][storage]") {
    auto memory = counting_memory_resource{};
    auto storage = bg::details::time_series_set_storage<std::int64_t>{};
    const auto existing_key = bg::ScalarValue<std::int64_t>{1};
    const auto new_key = bg::ScalarValue<std::int64_t>{2};
    auto constructed_items = 0;
    const auto first_time = bg::MIN_DT + bg::EvaluationTime::duration{1};

    storage.reset_change_tracking(first_time, bg::MIN_DT, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto existing = storage.insert(
        existing_key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });
    storage.reset_change_tracking(first_time + bg::EvaluationTime::duration{1}, first_time, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    const auto added = storage.insert(
        new_key.input_view(), memory, [&](std::uint32_t) { ++constructed_items; },
        [&](std::uint32_t) noexcept { --constructed_items; });

    storage.clear(memory, [&](std::uint32_t) noexcept { --constructed_items; });

    CHECK(storage.size() == 0);
    CHECK(storage.removed().contains(existing.position));
    CHECK(storage.find_position(existing_key.input_view()) == existing.position);
    CHECK(storage.find_position(new_key.input_view()) == bg::util::occupancy_index::npos);
    CHECK_FALSE(storage.removed().contains(added.position));
    CHECK(constructed_items == 1);

    storage.reset_change_tracking(first_time + bg::EvaluationTime::duration{2}, first_time, memory,
                                  [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(constructed_items == 0);

    storage.release(memory, [&](std::uint32_t) noexcept { --constructed_items; });
    CHECK(memory.allocations() == 0);
}
