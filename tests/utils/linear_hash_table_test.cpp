#include <bg/util/linear_hash_table.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("a linear hash table finds and removes positions", "[linear_hash_table]") {
    auto memory = counting_memory_resource{};
    auto table = bg::util::linear_hash_table{};

    const auto first_hash = bg::util::linear_hash_table::hash_code(10);
    const auto second_hash = bg::util::linear_hash_table::hash_code(20);
    table.insert(first_hash, 3, memory);
    table.insert(second_hash, 7, memory);

    CHECK(table.find(first_hash, [](auto position) { return position == 3; }) == 3);
    CHECK_FALSE(table.find(first_hash, [](auto position) { return position == 7; }).has_value());
    CHECK(table.erase(first_hash, 3, memory));
    CHECK_FALSE(table.erase(first_hash, 3, memory));

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table retains entries while splitting buckets", "[linear_hash_table]") {
    auto memory = counting_memory_resource{};
    auto table = bg::util::linear_hash_table{};

    auto previous_buckets = std::uint32_t{};
    for (auto position = std::uint32_t{}; position < 200; ++position) {
        table.insert(bg::util::linear_hash_table::hash_code(position), position, memory);
        CHECK(table.bucket_count() <= previous_buckets + 1);
        previous_buckets = table.bucket_count();
    }

    for (auto position = std::uint32_t{}; position < 200; ++position) {
        const auto found = table.find(bg::util::linear_hash_table::hash_code(position),
                                      [position](auto candidate) { return candidate == position; });
        CHECK(found == position);
    }

    table.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a linear hash table uses compact overflow buckets for equal hashes", "[linear_hash_table]") {
    auto memory = counting_memory_resource{};
    auto table = bg::util::linear_hash_table{};
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

TEST_CASE("a linear hash table merges one bucket at a time", "[linear_hash_table]") {
    auto memory = counting_memory_resource{};
    auto table = bg::util::linear_hash_table{};

    for (auto position = std::uint32_t{}; position < 100; ++position) {
        table.insert(bg::util::linear_hash_table::hash_code(position), position, memory);
    }
    for (auto position = std::uint32_t{}; position < 80; ++position) {
        CHECK(table.erase(bg::util::linear_hash_table::hash_code(position), position, memory));
    }

    while (true) {
        const auto previous_buckets = table.bucket_count();
        if (!table.reduce(memory)) {
            break;
        }

        CHECK(table.bucket_count() + 1 == previous_buckets);
    }

    for (auto position = std::uint32_t{80}; position < 100; ++position) {
        CHECK(table.find(bg::util::linear_hash_table::hash_code(position),
                         [position](auto candidate) { return candidate == position; }) == position);
    }

    table.release(memory);
    CHECK(memory.allocations() == 0);
}
