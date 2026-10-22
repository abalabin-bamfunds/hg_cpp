#include <bg/util/occupancy_index.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace bg;

TEST_CASE("an occupancy index allocates and reuses positions", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};

    for (auto expected = std::uint32_t{}; expected < 70; ++expected) {
        CHECK(index.acquire(memory) == expected);
    }

    CHECK(index.reset(17));
    CHECK(index.acquire(memory) == 17);
    CHECK(index.size() == 70);

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an occupancy index stores its first 56 positions without allocation", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};

    for (auto expected = std::uint32_t{}; expected < 56; ++expected) {
        CHECK(index.acquire(memory) == expected);
    }

    CHECK(memory.allocations() == 0);
    CHECK(index.acquire(memory) == 56);
    CHECK(memory.allocations() == 1);

    index.release(memory);
}

TEST_CASE("an occupancy index supports explicit sparse positions", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};

    CHECK(index.set(130, memory));
    CHECK(index.set(2, memory));
    CHECK_FALSE(index.set(130, memory));

    const auto positions = std::vector<std::uint32_t>{index.begin(), index.end()};
    CHECK(positions == std::vector<std::uint32_t>{2, 130});

    CHECK(index.reset(130));
    index.trim(memory);
    CHECK(index.capacity() == 56);

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an occupancy index handles distant positions and word boundaries", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};

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

TEST_CASE("an occupancy index retains its state when allocation fails", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};

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

TEST_CASE("an occupancy index enumerates live position runs", "[occupancy_index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::occupancy_index{};
    auto run_count = std::size_t{};

    index.for_each_run([&](std::uint32_t, std::uint32_t) { ++run_count; });
    CHECK(run_count == 0);

    for (auto position = std::uint32_t{}; position != 70; ++position) {
        REQUIRE(index.set(position, memory));
    }

    auto runs = std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{};
    index.for_each_run([&](std::uint32_t first, std::uint32_t end) { runs[run_count++] = {first, end}; });
    REQUIRE(run_count == 1);
    CHECK(runs[0] == std::pair{std::uint32_t{0}, std::uint32_t{70}});

    for (const auto position : std::array{std::uint32_t{1}, std::uint32_t{2}, std::uint32_t{67}, std::uint32_t{68}}) {
        REQUIRE(index.reset(position));
    }
    for (const auto position : std::array{std::uint32_t{127}, std::uint32_t{128}}) {
        REQUIRE(index.set(position, memory));
    }

    run_count = 0;
    const auto allocations = memory.allocations();
    index.for_each_run([&](std::uint32_t first, std::uint32_t end) { runs[run_count++] = {first, end}; });
    CHECK(memory.allocations() == allocations);
    REQUIRE(run_count == 4);
    CHECK(runs[0] == std::pair{std::uint32_t{0}, std::uint32_t{1}});
    CHECK(runs[1] == std::pair{std::uint32_t{3}, std::uint32_t{67}});
    CHECK(runs[2] == std::pair{std::uint32_t{69}, std::uint32_t{70}});
    CHECK(runs[3] == std::pair{std::uint32_t{127}, std::uint32_t{129}});

    index.release(memory);
    CHECK(memory.allocations() == 0);
}
