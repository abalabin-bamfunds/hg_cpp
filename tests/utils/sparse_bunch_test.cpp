#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_CASE("sparse bunches share one occupancy index", "[sparse_bunch]") {
    auto memory = counting_memory_resource{};
    auto positions = bg::util::occupancy_index{};
    auto keys = bg::util::sparse_bunch<std::uint32_t>{};
    auto values = bg::util::sparse_bunch<std::string>{};

    for (auto position = std::uint32_t{}; position < 9; ++position) {
        keys.emplace_at(position, memory, position);
        values.emplace_at(position, memory, std::to_string(position));
        CHECK(positions.set(position, memory));
    }

    auto *first_key = keys.get(0, positions);
    keys.emplace_at(40, memory, 40);
    values.emplace_at(40, memory, "40");
    CHECK(positions.set(40, memory));

    CHECK(keys.get(0, positions) == first_key);
    CHECK(*values.get(40, positions) == "40");

    for (auto position = std::uint32_t{}; position < 8; ++position) {
        keys.erase_at(position, memory);
        values.erase_at(position, memory);
        CHECK(positions.reset(position));
    }

    CHECK(keys.allocated_block_count() == 2);
    CHECK(values.allocated_block_count() == 2);
    CHECK(keys.get(0, positions) == nullptr);

    keys.release(positions, memory);
    values.release(positions, memory);
    positions.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a sparse bunch releases an empty element block", "[sparse_bunch]") {
    auto memory = counting_memory_resource{};
    auto positions = bg::util::occupancy_index{};
    auto values = bg::util::sparse_bunch<std::uint32_t>{};

    values.emplace_at(0, memory, 10);
    values.emplace_at(8, memory, 20);
    CHECK(positions.set(0, memory));
    CHECK(positions.set(8, memory));
    REQUIRE(values.allocated_block_count() == 2);

    values.erase_at(0, memory);
    CHECK(positions.reset(0));
    CHECK(values.allocated_block_count() == 1);

    values.release(positions, memory);
    positions.release(memory);
    CHECK(memory.allocations() == 0);
}
