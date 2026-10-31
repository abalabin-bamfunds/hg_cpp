#include <bg/util/adaptive_set.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <vector>

using namespace bg;

namespace {

struct test_key {
    test_key(std::string_view value) : value{value} {}

    std::string_view value;
};

struct test_key_hash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
    std::size_t operator()(test_key value) const noexcept { return operator()(value.value); }
};

struct test_key_equal {
    bool operator()(test_key left, std::string_view right) const noexcept { return left.value == right; }
    bool operator()(test_key left, test_key right) const noexcept { return left.value == right.value; }
};

} // namespace

TEST_CASE("an adaptive set inserts, finds, and removes keys", "[adaptive_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = util::adaptive_set<test_key, 4, test_key_hash, test_key_equal>{};

    const auto [first, first_inserted] = set.insert(test_key{"one"}, memory);
    const auto [same, second_inserted] = set.insert(test_key{"one"}, memory);
    set.insert(test_key{"two"}, memory);

    CHECK(first_inserted);
    CHECK_FALSE(second_inserted);
    CHECK(first == same);
    CHECK(set.contains(std::string_view{"one"}));
    CHECK(set.find(std::string_view{"missing"}) == nullptr);
    CHECK(set.erase(std::string_view{"one"}, memory));
    CHECK_FALSE(set.erase(std::string_view{"one"}, memory));

    set.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an adaptive set retains key storage when it creates its index", "[adaptive_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = util::adaptive_set<std::uint32_t, 4>{};
    const auto *first = set.insert(std::uint32_t{1}, memory).first;

    set.insert(std::uint32_t{2}, memory);
    set.insert(std::uint32_t{3}, memory);
    set.insert(std::uint32_t{4}, memory);

    CHECK(set.find(std::uint32_t{1}) == first);

    set.insert(std::uint32_t{5}, memory);
    CHECK(set.find(std::uint32_t{1}) == first);

    auto values = std::vector<int>{set.begin(), set.end()};
    CHECK(values.size() == 5);

    set.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an adaptive set rolls back an insertion when index creation fails", "[adaptive_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = util::adaptive_set<std::uint32_t, 2>{};

    set.insert(std::uint32_t{1}, memory);
    set.insert(std::uint32_t{2}, memory);
    memory.fail_after(0);

    CHECK_THROWS_AS(set.insert(std::uint32_t{3}, memory), std::bad_alloc);
    CHECK(set.size() == 2);
    CHECK(set.contains(std::uint32_t{1}));
    CHECK(set.contains(std::uint32_t{2}));
    CHECK_FALSE(set.contains(std::uint32_t{3}));

    memory.allow_allocations();
    CHECK(set.insert(std::uint32_t{3}, memory).second);

    set.release(memory);
    CHECK(memory.allocations() == 0);
}

namespace {

struct equal_hash {
    template <typename T> std::size_t operator()(const T &) const noexcept { return 1; }
};

} // namespace

TEST_CASE("an adaptive set resolves collisions before and after indexing", "[adaptive_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = util::adaptive_set<std::uint32_t, 4, equal_hash>{};

    for (auto value = std::uint32_t{}; value < 40; ++value) {
        set.insert(value, memory);
    }
    for (auto value = std::uint32_t{}; value < 40; ++value) {
        CHECK(set.contains(value));
    }
    for (auto value = std::uint32_t{1}; value < 40; value += 2) {
        CHECK(set.erase(value, memory));
    }

    auto values = std::vector<std::uint32_t>{set.begin(), set.end()};
    for (auto value = std::uint32_t{}; value < 40; value += 2) {
        CHECK(values[value / 2] == value);
        CHECK(set.erase(value, memory));
    }

    CHECK(set.empty());

    CHECK(set.insert(std::uint32_t{41}, memory).second);
    CHECK(set.erase(std::uint32_t{41}, memory));

    set.release(memory);
    CHECK(memory.allocations() == 0);
}
