#include <bg/util/index.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

using namespace bg;

TEST_CASE("an empty fixed-size index has no set positions", "[index][coverage-exempt]") {
    const auto index = util::index<0>{};

    CHECK(index.empty());
    CHECK(index.all());
    CHECK(index.begin() == index.end());
}

TEST_CASE("a runtime-sized index stores up to 56 positions without allocation", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::index<>{};

    for (auto position = std::uint32_t{}; position < util::index<>::inline_capacity; ++position) {
        CHECK(index.set(position, memory));
    }

    CHECK(index.size() == 56);
    CHECK(index.count() == 56);
    CHECK(index.all());
    CHECK(memory.allocations() == 0);

    index.release(memory);
}

TEST_CASE("a runtime-sized index preserves bits when it starts using allocated storage",
          "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::index<>{};

    CHECK(index.set(0, memory));
    CHECK(index.set(55, memory));
    CHECK(index.set(56, memory));

    CHECK(memory.allocations() == 1);
    CHECK(index.size() == 57);
    CHECK(index.contains(0));
    CHECK(index.contains(55));
    CHECK(index.contains(56));
    CHECK(index.count() == 3);

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a runtime-sized index grows, resets, clears, and iterates set positions",
          "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::index<>{};

    for (const auto position : {std::uint32_t{0}, std::uint32_t{63}, std::uint32_t{64}, std::uint32_t{511}}) {
        CHECK(index.set(position, memory));
    }

    CHECK_FALSE(index.set(64, memory));
    CHECK(index.reset(63));
    CHECK_FALSE(index.reset(63));
    CHECK(index.first_set() == 0);
    CHECK(index.first_unset() == 1);
    CHECK(std::vector<std::uint32_t>{index.begin(), index.end()} == std::vector<std::uint32_t>{0, 64, 511});

    index.clear();
    CHECK(index.size() == 512);
    CHECK(index.empty());

    index.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an index finds the next set position", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto dynamic = util::index<>{};
    CHECK(dynamic.set(3, memory));
    CHECK(dynamic.set(64, memory));
    CHECK(dynamic.set(130, memory));

    CHECK(dynamic.first_set(0) == 3);
    CHECK(dynamic.first_set(4) == 64);
    CHECK(dynamic.first_set(65) == 130);
    CHECK(dynamic.first_set(131) == dynamic.size());
    dynamic.release(memory);

    auto fixed = util::index<129>{};
    CHECK(fixed.set(3));
    CHECK(fixed.set(64));
    CHECK(fixed.set(128));
    CHECK(fixed.first_set(4) == 64);
    CHECK(fixed.first_set(65) == 128);
    CHECK(fixed.first_set(129) == fixed.size());
}

TEST_CASE("an index finds the first set position in either index", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto empty = util::index<>{};
    CHECK(empty.first_set_union(empty) == 0);
    CHECK(empty.first_set_union(empty, 1) == 0);

    auto inline_left = util::index<>{};
    auto inline_right = util::index<>{};
    CHECK(inline_left.set(3, memory));
    CHECK(inline_right.set(55, memory));
    CHECK(inline_left.first_set_union(inline_right) == 3);
    CHECK(inline_left.first_set_union(inline_right, 4) == 55);
    CHECK(inline_left.first_set_union(inline_right, 56) == 56);
    CHECK(inline_left.first_set_union(inline_right, 60) == 56);

    auto mixed_inline = util::index<>{};
    auto mixed_allocated = util::index<>{};
    CHECK(mixed_inline.set(2, memory));
    CHECK(mixed_inline.set(55, memory));
    CHECK(mixed_allocated.set(2, memory));
    CHECK(mixed_allocated.set(64, memory));
    CHECK(mixed_allocated.set(129, memory));
    CHECK(mixed_inline.first_set_union(mixed_allocated) == 2);
    CHECK(mixed_inline.first_set_union(mixed_allocated, 3) == 55);
    CHECK(mixed_inline.first_set_union(mixed_allocated, 56) == 64);
    CHECK(mixed_inline.first_set_union(mixed_allocated, 65) == 129);
    mixed_allocated.release(memory);

    auto allocated_left = util::index<>{};
    auto allocated_right = util::index<>{};
    CHECK(allocated_left.set(63, memory));
    CHECK(allocated_left.set(130, memory));
    CHECK(allocated_right.set(64, memory));
    CHECK(allocated_right.set(511, memory));
    CHECK(allocated_left.first_set_union(allocated_right) == 63);
    CHECK(allocated_left.first_set_union(allocated_right, 64) == 64);
    CHECK(allocated_left.first_set_union(allocated_right, 65) == 130);
    CHECK(allocated_left.first_set_union(allocated_right, 131) == 511);
    CHECK(allocated_left.first_set_union(allocated_right, 512) == 512);

    allocated_left.release(memory);
    allocated_right.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a bounded unset search clips the range and treats missing bits as unset", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto dynamic = util::index<>{};
    for (auto position = std::uint32_t{}; position != 56; ++position) {
        CHECK(dynamic.set(position, memory));
    }

    CHECK(dynamic.first_unset(53, 56) == 56);
    CHECK(dynamic.first_unset(55, 57) == 56);

    CHECK(dynamic.set(56, memory));
    for (auto position = std::uint32_t{57}; position != 130; ++position) {
        if (position != 64 && position != 95) {
            CHECK(dynamic.set(position, memory));
        }
    }

    CHECK(dynamic.first_unset(63, 64) == 64);
    CHECK(dynamic.first_unset(63, 65) == 64);
    CHECK(dynamic.set(64, memory));
    CHECK(dynamic.first_unset(64, 95) == 95);
    CHECK(dynamic.first_unset(64, 96) == 95);
    CHECK(dynamic.first_unset(3, 128) == 95);
    CHECK(dynamic.first_unset(128, 140) == 130);
    CHECK(dynamic.first_unset(132, 140) == 132);
    CHECK(dynamic.first_unset(132) == dynamic.size());
    CHECK(dynamic.first_unset(130, 130) == 130);
    CHECK(dynamic.first_unset(131, 130) == 130);

    dynamic.release(memory);
}

TEST_CASE("bounded unset search scans full words and handles maximum bounds", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto dynamic = util::index<>{};
    for (auto position = std::uint32_t{}; position != 129; ++position) {
        CHECK(dynamic.set(position, memory));
    }

    for (auto start = std::uint32_t{}; start != dynamic.size(); ++start) {
        for (auto width = std::uint32_t{1}; width <= 3; ++width) {
            const auto end = std::min<std::uint32_t>(start + width, dynamic.size());
            CHECK(dynamic.first_unset(start, end) == end);
        }
    }

    CHECK(dynamic.first_unset(0, 129) == 129);
    CHECK(dynamic.first_unset(63, 66) == 66);
    CHECK(dynamic.first_unset(127, 200) == 129);
    CHECK(dynamic.first_unset(129, 200) == 129);
    dynamic.release(memory);

    auto empty = util::index<>{};
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    CHECK(empty.first_unset(maximum - 1, maximum) == maximum - 1);
    CHECK(empty.first_unset(maximum, maximum - 1) == maximum - 1);
}

TEST_CASE("a runtime-sized index retains its state when growth fails", "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::index<>{};

    CHECK(index.set(4, memory));
    memory.fail_after(0);
    CHECK_THROWS_AS(index.set(100, memory), std::bad_alloc);
    memory.allow_allocations();

    CHECK(index.size() == 5);
    CHECK(index.count() == 1);
    CHECK(index.contains(4));
    CHECK_FALSE(index.contains(100));

    index.release(memory);
}

TEST_CASE("a runtime-sized index returns to inline storage after trimming",
          "[index][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto index = util::index<>{};

    CHECK(index.set(100, memory));
    CHECK(index.set(12, memory));
    CHECK(index.reset(100));
    index.trim(memory);

    CHECK(index.size() == 13);
    CHECK(index.capacity() == 56);
    CHECK(index.contains(12));
    CHECK(memory.allocations() == 0);

    index.release(memory);
}

TEST_CASE("a fixed-size index crosses word boundaries without allocation", "[index][coverage-exempt]") {
    auto index = util::index<129>{};

    CHECK(index.set(0));
    CHECK(index.set(63));
    CHECK(index.set(64));
    CHECK(index.set(128));
    CHECK_FALSE(index.set(64));

    CHECK(index.count() == 4);
    CHECK(index.first_set() == 0);
    CHECK(std::vector<std::uint32_t>{index.begin(), index.end()} == std::vector<std::uint32_t>{0, 63, 64, 128});

    CHECK(index.reset(64));
    CHECK_FALSE(index.contains(64));
    index.clear();
    CHECK(index.empty());
    CHECK_THROWS_AS(index.set(129), std::out_of_range);
}
