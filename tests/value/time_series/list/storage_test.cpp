#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

TEST_CASE("fixed list changes cross bitset word boundaries", "[value][time_series][tsl]") {
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

TEST_CASE("fixed list validity is derived from its items", "[value][time_series][tsl]") {
    auto storage = bg::details::fixed_time_series_list_storage<bg::TS<int>, 3>{};
    CHECK_FALSE(storage.valid());

    storage.items[1].last_updated = bg::MIN_DT + std::chrono::microseconds{1};
    CHECK(storage.valid());

    storage.items[1].last_updated = bg::MIN_DT;
    CHECK_FALSE(storage.valid());
}

TEST_CASE("fixed list storage reports item cleanup requirements", "[value][time_series][tsl][storage]") {
    const auto &storage = bg::cpp_time_series_storage_representation<bg::TSL<bg::TS<int>, 2>>();

    CHECK_FALSE(storage.uses_variable_memory());
    CHECK_FALSE(storage.requires_finalization());
}
