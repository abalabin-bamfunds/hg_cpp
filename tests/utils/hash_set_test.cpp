#include <bg/util/hash_set.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct string_hash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

} // namespace

TEST_CASE("a hash set inserts, finds, and removes keys",
          "[hash_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = bg::util::hash_set<std::string, string_hash>{};

    const auto [first, first_inserted] = set.insert(std::string{"one"}, memory);
    const auto [same, second_inserted] = set.insert(std::string{"one"}, memory);
    set.insert(std::string{"two"}, memory);

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

TEST_CASE("a hash set retains key addresses while it grows", "[hash_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = bg::util::hash_set<int>{};
    const auto *first = set.insert(1, memory).first;

    for (auto value = 2; value < 500; ++value) {
        set.insert(value, memory);
    }

    CHECK(set.find(1) == first);

    auto values = std::vector<int>{set.begin(), set.end()};
    CHECK(values.size() == 499);

    set.release(memory);
    CHECK(memory.allocations() == 0);
}

namespace {

struct equal_hash {
    template <typename T> [[nodiscard]] std::size_t operator()(const T &) const noexcept { return 1; }
};

} // namespace

TEST_CASE("a hash set resolves collisions", "[hash_set][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto set = bg::util::hash_set<int, equal_hash>{};

    for (auto value = 0; value < 40; ++value) {
        set.insert(value, memory);
    }
    for (auto value = 0; value < 40; ++value) {
        CHECK(set.contains(value));
    }

    set.release(memory);
    CHECK(memory.allocations() == 0);
}
