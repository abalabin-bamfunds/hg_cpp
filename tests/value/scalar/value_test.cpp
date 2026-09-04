#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

TEST_CASE("a scalar value maps signed integers to int64", "[value][scalar][feature:scalar-api][scalar:integer]") {
    const auto value = bg::ScalarValue{42};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a scalar value accepts lossless unsigned integer conversion",
          "[value][scalar][feature:scalar-api][scalar:integer]") {
    const auto value = bg::ScalarValue{42U};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a pointer-sized scalar value uses inline storage",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    const auto value = bg::ScalarValue{42, *std::pmr::null_memory_resource()};

    CHECK(&value.view().runtime_type() == &bg::cpp_scalar_runtime_type<std::int64_t>());
    CHECK(value.view().value() == 42);
}

TEST_CASE("a pointer-sized scalar value copies a borrowed value inline",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    const auto source_value = std::int64_t{42};
    const auto source = bg::Scalar<std::int64_t>::from(source_value);
    const auto value = bg::ScalarValue<std::int64_t>{source, *std::pmr::null_memory_resource()};

    CHECK(value.view().value() == source_value);
}

TEST_CASE("pointer-sized scalar values compare and hash from inline storage",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    const auto left = bg::ScalarValue{42};
    const auto equal = bg::ScalarValue{42};
    const auto different = bg::ScalarValue{43};

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left.view().hash() == equal.view().hash());
}

TEST_CASE("an evaluation time scalar value retains its graph type",
          "[value][scalar][feature:scalar-api][scalar:evaluation-time]") {
    const auto source = bg::MIN_DT + std::chrono::microseconds{42};
    const auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::EvaluationTime>>);
    CHECK(&value.view().type() == &bg::evaluation_time_type());
    CHECK(value.view().value() == source);
}

TEST_CASE("a scalar value owns character data",
          "[value][scalar][feature:scalar-api][scalar:string][storage:variable]") {
    auto source = std::string{"value"};
    auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), bg::ScalarValue<bg::string>>);
    source.assign("changed");

    CHECK(value.view().value() == "value");
}

TEST_CASE("a scalar value accepts a string literal", "[value][scalar][feature:scalar-api][scalar:string]") {
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

TEST_CASE("a scalar value uses the supplied memory resource", "[value][scalar][feature:scalar-api]") {
    auto memory = counting_memory_resource{};

    {
        auto value = bg::ScalarValue{std::string_view{"long enough to require separate character storage"}, memory};
        CHECK(value.view().value() == "long enough to require separate character storage");
    }

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("a scalar value uses the current node memory resource", "[value][scalar][feature:scalar-api]") {
    auto memory = counting_memory_resource{};
    const auto graph = 1;
    {
        const auto context = bg::details::ScalarAllocationContext{memory, &graph, 2};
        const auto value = bg::ScalarValue{"long enough to require separate character storage"};

        CHECK(value.view().value() == "long enough to require separate character storage");
    }

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("a scalar value transfers ownership when moved", "[value][scalar][feature:scalar-api]") {
    auto source = bg::ScalarValue{std::string_view{"value"}};
    auto destination = std::move(source);

    CHECK(destination.view().value() == "value");
}
