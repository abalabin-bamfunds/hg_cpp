#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

using namespace bg;

namespace {

struct value_from_scalar {
    explicit value_from_scalar(const Scalar<i64> &source) : number{source.value()} {}

    i64 number;
};

} // namespace

TEST_CASE("a scalar value maps signed integers to int64", "[value][scalar][feature:scalar-api][scalar:integer]") {
    const auto value = ScalarValue{42};

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<i64>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a scalar value accepts lossless unsigned integer conversion",
          "[value][scalar][feature:scalar-api][scalar:integer]") {
    const auto value = ScalarValue{42U};

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<i64>>);
    CHECK(value.view().value() == 42);
}

TEST_CASE("a pointer-sized scalar value uses inline storage",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    auto allocator = bg::allocator{*std::pmr::null_memory_resource()};
    const auto value = ScalarValue{42, allocator};

    CHECK(&value.view().runtime_type() == &cpp_scalar_runtime_type<i64>());
    CHECK(value.view().value() == 42);
}

TEST_CASE("a pointer-sized scalar value copies a borrowed value inline",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    const auto source_value = i64{42};
    const auto source = Scalar<i64>::from(source_value);
    auto allocator = bg::allocator{*std::pmr::null_memory_resource()};
    const auto value = ScalarValue<i64>{source, allocator};

    CHECK(value.view().value() == source_value);
}

TEST_CASE("a scalar value uses its provider's cross-declaration construction",
          "[value][scalar][feature:scalar-api][declaration:concrete]") {
    const auto source = ScalarValue{42};
    const auto value = ScalarValue<Opaque<value_from_scalar>>{source.view()};

    CHECK(value.view()->number == 42);
}

TEST_CASE("pointer-sized scalar values compare and hash from inline storage",
          "[value][scalar][feature:scalar-api][scalar:integer][storage:inline]") {
    const auto left = ScalarValue{42};
    const auto equal = ScalarValue{42};
    const auto different = ScalarValue{43};

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left.equals(equal));
    CHECK(left.hash() == equal.hash());
}

TEST_CASE("scalar hash and equality support heterogeneous scalar handles",
          "[value][scalar][feature:scalar-api][scalar:integer]") {
    const auto stored = ScalarValue<>{42};
    const auto equal = ScalarValue{42};
    const auto different = ScalarValue{43};

    CHECK(ScalarHash{}(stored) == ScalarHash{}(equal.view()));
    CHECK(ScalarEqual{}(stored, equal.view()));
    CHECK(ScalarEqual{}(equal.view(), stored));
    CHECK_FALSE(ScalarEqual{}(stored, different.view()));
}

TEST_CASE("an evaluation time scalar value retains its graph type",
          "[value][scalar][feature:scalar-api][scalar:evaluation-time]") {
    const auto source = MIN_DT + std::chrono::microseconds{42};
    const auto value = ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<EvaluationTime>>);
    CHECK(&value.view().type() == &evaluation_time_type());
    CHECK(value.view().value() == source);
}

TEST_CASE("a scalar value owns character data",
          "[value][scalar][feature:scalar-api][scalar:string][storage:variable]") {
    auto source = std::string{"value"};
    auto value = ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), ScalarValue<string>>);
    source.assign("changed");

    CHECK(value.view().value() == "value");
}

TEST_CASE("a scalar value accepts a string literal", "[value][scalar][feature:scalar-api][scalar:string]") {
    const auto value = ScalarValue{"value"};

    STATIC_CHECK(std::same_as<decltype(value), const ScalarValue<string>>);
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
        auto allocator = bg::allocator{memory};
        auto value = ScalarValue{std::string_view{"long enough to require separate character storage"}, allocator};
        CHECK(value.view().value() == "long enough to require separate character storage");
    }

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("a scalar value uses the current node memory resource", "[value][scalar][feature:scalar-api]") {
    auto memory = counting_memory_resource{};
    const auto graph = 1;
    auto *scalar_allocation_pool = details::ScalarAllocationPool::create(allocator{memory});
    {
        const auto context = details::ScalarAllocationContext{allocator{memory}, *scalar_allocation_pool, &graph, 2};
        const auto value = ScalarValue{"long enough to require separate character storage"};

        CHECK(value.view().value() == "long enough to require separate character storage");
    }
    scalar_allocation_pool->release();

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("a scalar value transfers ownership when moved", "[value][scalar][feature:scalar-api]") {
    auto source = ScalarValue{std::string_view{"value"}};
    auto destination = std::move(source);

    CHECK(destination.view().value() == "value");
}
