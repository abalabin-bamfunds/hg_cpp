#include <bg/util/hash_dictionary.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

struct string_hash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct equal_hash {
    template <typename T> [[nodiscard]] std::size_t operator()(const T &) const noexcept { return 1; }
};

struct throwing_value {
    explicit throwing_value(bool fail) {
        if (fail) {
            throw std::runtime_error{"construction failed"};
        }
    }
};

} // namespace

TEST_CASE("a hash dictionary inserts, replaces, and finds values", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<std::string, int, string_hash>{};

    const auto [first, first_inserted] = dictionary.try_emplace(std::string{"one"}, memory, 1);
    const auto [same, second_inserted] = dictionary.try_emplace(std::string{"one"}, memory, 2);

    CHECK(first_inserted);
    CHECK_FALSE(second_inserted);
    CHECK(first == same);
    CHECK(*same == 1);
    CHECK(dictionary.insert_or_assign(std::string{"one"}, 3, memory) == 3);
    CHECK(dictionary.at(std::string_view{"one"}) == 3);
    CHECK_THROWS_AS(dictionary.at(std::string_view{"missing"}), std::out_of_range);

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a hash dictionary retains separate key and value addresses", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<int, std::string>{};
    auto *first_value = dictionary.try_emplace(1, memory, "one").first;
    const auto *first_key = &(*dictionary.begin()).key;

    for (auto key = 2; key < 500; ++key) {
        static_cast<void>(dictionary.try_emplace(key, memory, std::to_string(key)));
    }

    CHECK(dictionary.find(1) == first_value);
    CHECK(&(*dictionary.begin()).key == first_key);

    const auto &constant = dictionary;
    auto count = std::size_t{};
    for ([[maybe_unused]] const auto item : constant) {
        ++count;
    }
    CHECK(count == 499);

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a hash dictionary rolls back a failed value construction", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<int, throwing_value>{};

    CHECK_THROWS_AS(dictionary.try_emplace(1, memory, true), std::runtime_error);
    CHECK(dictionary.empty());
    CHECK(dictionary.find(1) == nullptr);

    const auto [value, inserted] = dictionary.try_emplace(1, memory, false);
    CHECK(value != nullptr);
    CHECK(inserted);

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a hash dictionary reuses one position for its key and value", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<int, int>{};

    static_cast<void>(dictionary.try_emplace(1, memory, 10));
    static_cast<void>(dictionary.try_emplace(2, memory, 20));
    auto first = dictionary.begin();
    const auto *key_position = &(*first).key;
    const auto *value_position = &(*first).value;

    REQUIRE(dictionary.erase(1, memory));
    static_cast<void>(dictionary.try_emplace(3, memory, 30));

    auto replacement = dictionary.begin();
    CHECK((*replacement).key == 3);
    CHECK(&(*replacement).key == key_position);
    CHECK(&(*replacement).value == value_position);

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a hash dictionary supports move-only values and collisions", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<int, std::unique_ptr<int>, equal_hash>{};

    for (auto key = 0; key < 40; ++key) {
        static_cast<void>(dictionary.try_emplace(key, memory, std::make_unique<int>(key)));
    }

    auto replacement = std::make_unique<int>(100);
    auto &stored = dictionary.insert_or_assign(10, std::move(replacement), memory);
    CHECK(*stored == 100);
    CHECK(replacement == nullptr);
    CHECK(dictionary.erase(10, memory));
    CHECK_FALSE(dictionary.contains(10));

    for (auto key = 0; key < 40; ++key) {
        if (key != 10) {
            REQUIRE(dictionary.find(key) != nullptr);
            CHECK(**dictionary.find(key) == key);
        }
    }

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("a hash dictionary remains consistent across incremental growth and reduction", "[hash_dictionary]") {
    auto memory = counting_memory_resource{};
    auto dictionary = bg::util::hash_dictionary<int, int>{};
    auto expected = std::unordered_map<int, int>{};
    auto random = std::mt19937{12345};

    for (auto operation = 0; operation < 10'000; ++operation) {
        const auto key = static_cast<int>(random() % 300);
        const auto value = static_cast<int>(random());

        if ((random() & 1U) != 0) {
            dictionary.insert_or_assign(key, value, memory);
            expected.insert_or_assign(key, value);
        } else {
            CHECK(dictionary.erase(key, memory) == (expected.erase(key) != 0));
            static_cast<void>(dictionary.reduce(memory));
        }

        CHECK(dictionary.size() == expected.size());
        for (const auto &[expected_key, expected_value] : expected) {
            REQUIRE(dictionary.find(expected_key) != nullptr);
            CHECK(*dictionary.find(expected_key) == expected_value);
        }
    }

    dictionary.release(memory);
    CHECK(memory.allocations() == 0);
}
