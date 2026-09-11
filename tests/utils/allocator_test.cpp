#include <bg/allocator.hpp>

#include "counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

namespace {

struct Value {
    Value(int value, bool &destroyed) : value{value}, destroyed{&destroyed} {}
    ~Value() { *destroyed = true; }

    int value;
    bool *destroyed;
};

struct ConstructionFailure {
    ConstructionFailure() { throw std::runtime_error{"construction failed"}; }
};

} // namespace

TEST_CASE("an allocator creates and destroys an object", "[utils][allocator][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto allocation = bg::allocator{memory};
    auto destroyed = false;

    auto *value = allocation.new_object<Value>(42, destroyed);

    CHECK(value->value == 42);
    CHECK(memory.allocations() == 1);

    allocation.delete_object(value);

    CHECK(destroyed);
    CHECK(memory.allocations() == 0);
}

TEST_CASE("an allocator releases storage after construction fails", "[utils][allocator][coverage-exempt]") {
    auto memory = counting_memory_resource{};
    auto allocation = bg::allocator{memory};

    CHECK_THROWS_AS(allocation.new_object<ConstructionFailure>(), std::runtime_error);
    CHECK(memory.allocations() == 0);
}
