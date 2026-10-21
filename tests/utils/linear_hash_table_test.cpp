#include <bg/util/linear_hash_table.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using namespace bg;

TEST_CASE("a linear hash table finds and removes positions",
          "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};

    const auto first_hash = util::linear_hash_table::hash_code(10);
    const auto second_hash = util::linear_hash_table::hash_code(20);
    table.insert(first_hash, 3, memory);
    table.insert(second_hash, 7, memory);

    CHECK(table.find(first_hash, [](auto position) { return position == 3; }) == 3);
    CHECK_FALSE(table.find(first_hash, [](auto position) { return position == 7; }).has_value());
    CHECK(table.erase(first_hash, 3, memory));
    CHECK_FALSE(table.erase(first_hash, 3, memory));

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table retains high hash bits", "[linear_hash_table][coverage-exempt]") {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        const auto low = util::linear_hash_table::hash_code(std::size_t{1});
        const auto high = util::linear_hash_table::hash_code(std::size_t{1} << 32);

        CHECK(low != 0);
        CHECK(high != 0);
    }
}

TEST_CASE("a linear hash table retains entries while splitting buckets", "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};

    auto previous_buckets = std::uint32_t{};
    for (auto position = std::uint32_t{}; position < 200; ++position) {
        table.insert(util::linear_hash_table::hash_code(position), position, memory);
        CHECK(table.bucket_count() <= previous_buckets + 1);
        previous_buckets = table.bucket_count();
    }

    for (auto position = std::uint32_t{}; position < 200; ++position) {
        const auto found = table.find(util::linear_hash_table::hash_code(position),
                                      [position](auto candidate) { return candidate == position; });
        CHECK(found == position);
    }

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table uses compact overflow buckets for equal hashes",
          "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};
    constexpr auto hash = std::uint32_t{42};

    for (auto position = std::uint32_t{}; position < 10; ++position) {
        table.insert(hash, position, memory);
    }

    REQUIRE(table.overflow_bucket_count() == 1);
    for (auto position = std::uint32_t{}; position < 10; ++position) {
        CHECK(table.find(hash, [position](auto candidate) { return candidate == position; }) == position);
    }

    for (auto position = std::uint32_t{7}; position < 10; ++position) {
        CHECK(table.erase(hash, position, memory));
    }
    CHECK(table.overflow_bucket_count() == 0);

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table merges one bucket at a time", "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};

    for (auto position = std::uint32_t{}; position < 100; ++position) {
        table.insert(util::linear_hash_table::hash_code(position), position, memory);
    }
    for (auto position = std::uint32_t{}; position < 80; ++position) {
        CHECK(table.erase(util::linear_hash_table::hash_code(position), position, memory));
    }

    while (true) {
        const auto previous_buckets = table.bucket_count();
        if (!table.reduce(memory)) {
            break;
        }

        CHECK(table.bucket_count() + 1 == previous_buckets);
    }

    for (auto position = std::uint32_t{80}; position < 100; ++position) {
        CHECK(table.find(util::linear_hash_table::hash_code(position),
                         [position](auto candidate) { return candidate == position; }) == position);
    }

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table reduces collision-heavy storage", "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};
    constexpr auto hash = std::uint32_t{1};

    for (auto position = std::uint32_t{}; position < 100; ++position) {
        table.insert(hash, position, memory);
    }
    for (auto position = std::uint32_t{}; position < 90; ++position) {
        CHECK(table.erase(hash, position, memory));
    }

    auto reductions = std::uint32_t{};
    while (table.reduce(memory)) {
        ++reductions;
    }

    CHECK(reductions > 0);
    for (auto position = std::uint32_t{90}; position < 100; ++position) {
        CHECK(table.find(hash, [position](auto candidate) { return candidate == position; }) == position);
    }

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table retains entries when reduction allocation fails",
          "[linear_hash_table][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto table = util::linear_hash_table{};

    for (auto position = std::uint32_t{}; position < 500; ++position) {
        table.insert(position, position, memory);
    }

    memory.fail_after(0);
    auto first_remaining = std::uint32_t{};
    auto allocation_failed = false;
    for (; first_remaining < 500 && !allocation_failed; ++first_remaining) {
        CHECK(table.erase(first_remaining, first_remaining, memory));

        try {
            while (table.reduce(memory)) {
            }
        } catch (const std::bad_alloc &) {
            allocation_failed = true;
        }
    }
    memory.allow_allocations();

    REQUIRE(allocation_failed);
    CHECK(table.size() == 500 - first_remaining);
    for (auto position = first_remaining; position < 500; ++position) {
        CHECK(table.find(position, [position](auto candidate) { return candidate == position; }) == position);
    }
    CHECK(table.reduce(memory));

    table.release(memory);
    CHECK(memory.allocations() == 0);
}
