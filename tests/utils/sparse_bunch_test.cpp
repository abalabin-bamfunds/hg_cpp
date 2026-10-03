#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

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

TEST_CASE("a sparse bunch exposes raw storage block bounds", "[sparse_bunch][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto positions = bg::util::occupancy_index{};
    auto values = bg::util::sparse_bunch<std::uint32_t>{};

    const auto first = values.block_containing(7);
    const auto geometric = values.block_containing(8);
    const auto capped = values.block_containing(65'528);
    const auto last = values.block_containing(std::numeric_limits<std::uint32_t>::max());
    static_assert(std::is_same_v<decltype(first.data), const void *>);

    CHECK(first.first == 0);
    CHECK(first.end == 8);
    CHECK(first.data == nullptr);
    CHECK(geometric.first == 8);
    CHECK(geometric.end == 24);
    CHECK(geometric.data == nullptr);
    CHECK(capped.first == 65'528);
    CHECK(capped.end == 131'064);
    CHECK(capped.data == nullptr);
    CHECK(last.first <= std::numeric_limits<std::uint32_t>::max());
    CHECK(last.end == std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1);
    CHECK(last.data == nullptr);
    CHECK(memory.allocations() == 0);

    const auto bytes = bg::util::sparse_bunch<std::byte>{};
    const auto byte_capped = bytes.block_containing(262'136);
    CHECK(byte_capped.first == 262'136);
    CHECK(byte_capped.end == 524'280);

    auto &value = values.emplace_at(8, memory, 42);
    const auto allocated = values.block_containing(8);
    CHECK(allocated.data == &value);

    const auto allocations = memory.allocations();
    CHECK(values.block_containing(9).data == allocated.data);
    CHECK(memory.allocations() == allocations);

    values.erase_at(8, memory);
    CHECK(values.block_containing(8).data == nullptr);

    values.release(positions, memory);
    CHECK(memory.allocations() == 0);
}
