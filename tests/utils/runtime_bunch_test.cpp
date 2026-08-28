#include <bg/util/occupancy_index.hpp>
#include <bg/util/runtime_bunch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace {

struct alignas(32) item {
    std::uint64_t value;
    std::byte padding[24];
};

constexpr auto item_layout = bg::util::runtime_item_layout{sizeof(item), alignof(item)};

} // namespace

TEST_CASE("a runtime contiguous bunch stores a resolved item layout", "[util][bunch]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto bunch = bg::util::runtime_contiguous_bunch{};
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

TEST_CASE("a runtime sparse bunch shares external positions", "[util][bunch]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto positions = bg::util::occupancy_index{};
    auto bunch = bg::util::runtime_sparse_bunch{};

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

TEST_CASE("a runtime bunch releases storage after construction fails", "[util][bunch]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto bunch = bg::util::runtime_contiguous_bunch{};

    CHECK_THROWS_AS(bunch.emplace_back_with(item_layout, memory, [](void *) { throw std::runtime_error{"failed"}; }),
                    std::runtime_error);
    CHECK(bunch.empty());
    CHECK(bunch.allocated_block_count() == 0);
}
