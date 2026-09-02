#include <bg/time_series/tsl/tsl_storage.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory_resource>
#include <stdexcept>
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
    const auto &representation = bg::cpp_time_series_storage_representation<bg::TSL<bg::TS<int>, 3>>();
    auto memory = std::pmr::monotonic_buffer_resource{};
    auto *storage = memory.allocate(representation.value_size(), representation.value_alignment());
    representation.construct_runtime_value(storage, memory);

    CHECK_FALSE(representation.collection_valid(storage));

    const auto item = representation.fixed_item(1);
    auto &last_updated = *reinterpret_cast<bg::EvaluationTime *>(static_cast<std::byte *>(storage) +
                                                                item.last_updated_offset);
    last_updated = bg::MIN_DT + std::chrono::microseconds{1};
    CHECK(representation.collection_valid(storage));

    last_updated = bg::MIN_DT;
    CHECK_FALSE(representation.collection_valid(storage));

    representation.destroy_value(storage, memory);
}

TEST_CASE("fixed list storage reports item cleanup requirements", "[value][time_series][tsl][storage]") {
    const auto &storage = bg::cpp_time_series_storage_representation<bg::TSL<bg::TS<int>, 2>>();

    CHECK_FALSE(storage.uses_variable_memory());
    CHECK_FALSE(storage.requires_finalization());
}

TEST_CASE("variable-size list storage is not implemented", "[value][time_series][tsl][storage]") {
    const auto &item = bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &list = bg::time_series_list_type(item, std::dynamic_extent, bg::TimeSeriesGuarantee::ordinary);

    CHECK_THROWS_AS(bg::resolve_time_series_storage_representation(list), std::invalid_argument);
}
