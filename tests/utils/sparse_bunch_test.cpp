#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

TEST_CASE("sparse bunches share one occupancy index", "[sparse_bunch][coverage-exempt]") {
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

TEST_CASE("a sparse bunch releases an empty element block", "[sparse_bunch][coverage-exempt]") {
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

TEST_CASE("a sparse bunch handles growth and capped-block boundaries", "[sparse_bunch][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto positions = bg::util::occupancy_index{};
    auto values = bg::util::sparse_bunch<std::uint32_t>{};
    constexpr auto selected = std::array{std::uint32_t{0},      std::uint32_t{7},       std::uint32_t{8},
                                         std::uint32_t{23},     std::uint32_t{24},      std::uint32_t{65'527},
                                         std::uint32_t{65'528}, std::uint32_t{131'063}, std::uint32_t{131'064}};

    for (const auto position : selected) {
        values.emplace_at(position, memory, position + 1);
        CHECK(positions.set(position, memory));
    }

    for (const auto position : selected) {
        REQUIRE(values.get(position, positions) != nullptr);
        CHECK(*values.get(position, positions) == position + 1);
    }

    values.release(positions, memory);
    positions.release(memory);
    CHECK(memory.allocations() == 0);
}

namespace {

struct counted_value {
    explicit counted_value(std::uint32_t value) : value{value} { ++live; }
    ~counted_value() { --live; }

    counted_value(const counted_value &) = delete;
    counted_value &operator=(const counted_value &) = delete;

    std::uint32_t value;

    inline static std::uint32_t live{};
};

} // namespace

TEST_CASE("a sparse bunch destroys each occupied value once", "[sparse_bunch][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto positions = bg::util::occupancy_index{};
    auto values = bg::util::sparse_bunch<counted_value>{};

    for (const auto position : std::array{std::uint32_t{0}, std::uint32_t{8}, std::uint32_t{40}}) {
        values.emplace_at(position, memory, position);
        CHECK(positions.set(position, memory));
    }
    REQUIRE(counted_value::live == 3);

    values.erase_at(8, memory);
    CHECK(positions.reset(8));
    CHECK(counted_value::live == 2);

    values.release(positions, memory);
    positions.release(memory);
    CHECK(counted_value::live == 0);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a sparse bunch releases a constructed prefix", "[sparse_bunch][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto values = bg::util::sparse_bunch<counted_value, 1>{};

    for (auto position = std::uint32_t{}; position != 9; ++position) {
        values.emplace_at(position, memory, position);
    }
    REQUIRE(counted_value::live == 9);

    values.release_prefix(9, memory);
    CHECK(counted_value::live == 0);
    CHECK(memory.allocations() == 0);
}
