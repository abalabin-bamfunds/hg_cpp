#include <bg/util/occupancy_index.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_CASE("an occupancy index allocates and reuses positions", "[occupancy_index]") {
    auto memory = counting_memory_resource{};
    auto index = bg::util::occupancy_index{};

    for (auto expected = std::uint32_t{}; expected < 70; ++expected) {
        CHECK(index.acquire(memory) == expected);
    }

    CHECK(index.reset(17));
    CHECK(index.acquire(memory) == 17);
    CHECK(index.size() == 70);

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an occupancy index supports explicit sparse positions", "[occupancy_index]") {
    auto memory = counting_memory_resource{};
    auto index = bg::util::occupancy_index{};

    CHECK(index.set(130, memory));
    CHECK(index.set(2, memory));
    CHECK_FALSE(index.set(130, memory));

    const auto positions = std::vector<std::uint32_t>{index.begin(), index.end()};
    CHECK(positions == std::vector<std::uint32_t>{2, 130});

    CHECK(index.reset(130));
    index.trim(memory);
    CHECK(index.capacity() == 64);

    index.release(memory);
    CHECK(memory.allocations() == 0);
}
