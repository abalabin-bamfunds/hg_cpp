#include <bg/util/occupancy_index.hpp>
#include <bg/util/runtime_bunch.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace bg;

namespace {

struct alignas(32) item {
    std::uint64_t value;
    std::byte padding[24];
};

constexpr auto item_layout = util::runtime_item_layout{sizeof(item), alignof(item)};
constexpr auto byte_layout = util::runtime_item_layout{sizeof(std::byte), alignof(std::byte)};

} // namespace

TEST_CASE("a runtime contiguous bunch stores a resolved item layout", "[util][bunch][coverage-exempt]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto bunch = util::runtime_contiguous_bunch{};
    auto addresses = std::vector<const void *>{};

    for (auto value = std::uint64_t{}; value < 100; ++value) {
        addresses.push_back(bunch.emplace_back_with(
            item_layout, memory, [&](void *storage) { std::construct_at(static_cast<item *>(storage), value); }));
    }

    REQUIRE(bunch.size() == 100);
    for (auto index = std::uint32_t{}; index < bunch.size(); ++index) {
        CHECK(bunch.get(index, item_layout) == addresses[index]);
        CHECK(static_cast<const item *>(bunch.get(index, item_layout))->value == index);
    }

    bunch.release_with(item_layout, memory,
                       [](void *storage) noexcept { std::destroy_at(static_cast<item *>(storage)); });
}

TEST_CASE("a runtime sparse bunch shares external positions", "[util][bunch][coverage-exempt]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto positions = util::occupancy_index{};
    auto bunch = util::runtime_sparse_bunch{};

    for (const auto position : {std::uint32_t{2}, std::uint32_t{70}}) {
        REQUIRE(positions.set(position, memory));
        bunch.emplace_at_with(position, item_layout, memory,
                              [&](void *storage) { std::construct_at(static_cast<item *>(storage), position); });
    }

    CHECK(static_cast<const item *>(bunch.get(2, item_layout))->value == 2);
    CHECK(static_cast<const item *>(bunch.get(70, item_layout))->value == 70);

    bunch.release_with(positions, item_layout, memory,
                       [](void *storage) noexcept { std::destroy_at(static_cast<item *>(storage)); });
    positions.release(memory);
}

TEST_CASE("a runtime bunch releases storage after construction fails", "[util][bunch][coverage-exempt]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto bunch = util::runtime_contiguous_bunch{};

    CHECK_THROWS_AS(bunch.emplace_back_with(item_layout, memory, [](void *) { throw std::runtime_error{"failed"}; }),
                    std::runtime_error);
    CHECK(bunch.empty());
    CHECK(bunch.allocated_block_count() == 0);
}

TEST_CASE("a runtime sparse bunch exposes raw storage block bounds", "[util][bunch][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto bunch = util::runtime_sparse_bunch{};

    const auto first = bunch.block_containing(8, item_layout);
    const auto capped = bunch.block_containing(8'184, item_layout);
    const auto last = bunch.block_containing(std::numeric_limits<std::uint32_t>::max(), byte_layout);
    static_assert(std::is_same_v<decltype(first.data), const void *>);

    CHECK(first.first == 8);
    CHECK(first.end == 24);
    CHECK(first.data == nullptr);
    CHECK(capped.first == 8'184);
    CHECK(capped.end == 16'376);
    CHECK(capped.data == nullptr);
    CHECK(last.first <= std::numeric_limits<std::uint32_t>::max());
    CHECK(last.end == std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1);
    CHECK(last.data == nullptr);
    CHECK(memory.allocations() == 0);

    auto *item_storage = bunch.emplace_at_with(
        8, item_layout, memory, [](void *storage) { std::construct_at(static_cast<item *>(storage), item{42}); });
    const auto allocated = bunch.block_containing(8, item_layout);
    CHECK(allocated.data == item_storage);

    const auto allocations = memory.allocations();
    CHECK(bunch.block_containing(9, item_layout).data == allocated.data);
    CHECK(memory.allocations() == allocations);

    bunch.erase_at_with(8, item_layout, memory,
                        [](void *storage) noexcept { std::destroy_at(static_cast<item *>(storage)); });
    CHECK(bunch.block_containing(8, item_layout).data == nullptr);

    bunch.release(item_layout, memory);
    CHECK(memory.allocations() == 0);
}
