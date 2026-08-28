#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

TEST_CASE("a scalar value maps signed integers to int64", "[value][scalar]") {
    const auto value = bg::ScalarValue{42};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a scalar value accepts lossless unsigned integer conversion", "[value][scalar]") {
    const auto value = bg::ScalarValue{42U};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a pointer-sized scalar value uses inline storage", "[value][scalar]") {
    const auto value = bg::ScalarValue{42, *std::pmr::null_memory_resource()};

    CHECK(value.view().value() == 42);
}

TEST_CASE("an evaluation time scalar value retains its graph type", "[value][scalar]") {
    const auto source = bg::MIN_DT + std::chrono::microseconds{42};
    const auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::EvaluationTime>>);
    CHECK(&value.view().type() == &bg::evaluation_time_type());
    CHECK(value.view().value() == source);
}

TEST_CASE("a scalar value owns character data", "[value][scalar]") {
    auto source = std::string{"value"};
    auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), bg::ScalarValue<bg::string>>);
    source.assign("changed");

    CHECK(value.view().value() == "value");
}

TEST_CASE("a scalar value accepts a string literal", "[value][scalar]") {
    const auto value = bg::ScalarValue{"value"};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::string>>);
    CHECK(value.view().value() == "value");
}

namespace {

class counting_memory_resource final : public std::pmr::memory_resource {
  public:
    std::size_t allocations{};
    std::size_t deallocations{};

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *value, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }
};

} // namespace

TEST_CASE("a scalar value uses the supplied memory resource", "[value][scalar]") {
    auto memory = counting_memory_resource{};

    {
        auto value = bg::ScalarValue{std::string_view{"long enough to require separate character storage"}, memory};
        CHECK(value.view().value() == "long enough to require separate character storage");
    }

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("a scalar value uses the current node memory resource", "[value][scalar]") {
    auto memory = std::pmr::unsynchronized_pool_resource{};
    const auto graph = 1;
    const auto context = bg::details::ScalarAllocationContext{memory, &graph, 2};
    const auto value = bg::ScalarValue{"value"};

    CHECK(value.view().value() == "value");
}

TEST_CASE("a scalar value transfers ownership when moved", "[value][scalar]") {
    auto source = bg::ScalarValue{std::string_view{"value"}};
    auto destination = std::move(source);

    CHECK(destination.view().value() == "value");
}
