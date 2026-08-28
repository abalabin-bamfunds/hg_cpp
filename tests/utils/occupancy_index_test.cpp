#include <bg/util/occupancy_index.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
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

TEST_CASE("an occupancy index handles distant positions and word boundaries", "[occupancy_index]") {
    auto memory = counting_memory_resource{};
    auto index = bg::util::occupancy_index{};

    for (const auto position : std::array{std::uint32_t{0}, std::uint32_t{63}, std::uint32_t{64}, std::uint32_t{65},
                                          std::uint32_t{1'000'000}}) {
        CHECK(index.set(position, memory));
    }

    CHECK(index.contains(0));
    CHECK(index.contains(64));
    CHECK(index.contains(1'000'000));
    CHECK_FALSE(index.contains(999'999));
    CHECK_FALSE(index.reset(1'000'001));
    CHECK_THROWS_AS(index.set(std::numeric_limits<std::uint32_t>::max(), memory), std::length_error);

    const auto positions = std::vector<std::uint32_t>{index.begin(), index.end()};
    CHECK(positions == std::vector<std::uint32_t>{0, 63, 64, 65, 1'000'000});

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an occupancy index retains its state when allocation fails", "[occupancy_index]") {
    auto memory = counting_memory_resource{};
    auto index = bg::util::occupancy_index{};

    CHECK(index.set(1, memory));
    memory.fail_after(0);
    CHECK_THROWS_AS(index.set(1'000, memory), std::bad_alloc);
    memory.allow_allocations();

    CHECK(index.size() == 1);
    CHECK(index.contains(1));
    CHECK_FALSE(index.contains(1'000));

    index.release(memory);
    CHECK(memory.allocations() == 0);
}
