#include <bg/util/contiguous_bunch.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>

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

TEST_CASE("a contiguous bunch grows without moving its values", "[contiguous_bunch]") {
    auto memory = counting_memory_resource{};
    auto values = bg::util::contiguous_bunch<counted_value>{};

    auto *first = &values.emplace_back(memory, 0);
    for (auto value = std::uint32_t{1}; value < 40; ++value) {
        values.emplace_back(memory, value);
    }

    CHECK(&values[0] == first);
    CHECK(values[39].value == 39);
    CHECK(values.allocated_block_count() == 3);
    CHECK(counted_value::live == 40);

    while (values.size() > 24) {
        values.pop_back(memory);
    }

    CHECK(values.allocated_block_count() == 2);

    values.release(memory);
    CHECK(counted_value::live == 0);
    CHECK(memory.allocations() == 0);
}

namespace {

struct throwing_value {
    throwing_value() { throw std::runtime_error{"construction failed"}; }
};

} // namespace

TEST_CASE("a contiguous bunch releases storage after construction fails", "[contiguous_bunch]") {
    auto memory = counting_memory_resource{};
    auto values = bg::util::contiguous_bunch<throwing_value>{};

    CHECK_THROWS_AS(values.emplace_back(memory), std::runtime_error);
    CHECK(values.empty());
    CHECK(values.allocated_block_count() == 0);

    values.release(memory);
    CHECK(memory.allocations() == 0);
}
