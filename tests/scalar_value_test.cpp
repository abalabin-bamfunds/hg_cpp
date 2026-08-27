#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/list.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/list_scalar_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct private_data {
    explicit private_data(std::array<int, 4> source) : values(source) {}

    std::array<int, 4> values;

    private_data(const private_data &) = default;
    private_data &operator=(const private_data &) = delete;
};

template <typename T>
concept hashable_scalar_handle = requires(const T &value) { value->hash(); };

template <typename T>
concept equality_comparable_scalar_handle = requires(const T &left, const T &right) { left == right; };

using PrivateDataValue = bg::ScalarValue<bg::Opaque<private_data>>;
using PrivatePointerValue = bg::ScalarValue<bg::Opaque<private_data *>>;
using Key = bg::ScalarVariable<"Key", bg::EqualityComparable, bg::Hashable>;

static_assert(!hashable_scalar_handle<PrivateDataValue>);
static_assert(!hashable_scalar_handle<PrivatePointerValue>);
static_assert(!equality_comparable_scalar_handle<PrivateDataValue>);
static_assert(!equality_comparable_scalar_handle<PrivatePointerValue>);
static_assert(!hashable_scalar_handle<bg::Scalar<bg::SCALAR>>);
static_assert(!equality_comparable_scalar_handle<bg::Scalar<bg::SCALAR>>);
static_assert(hashable_scalar_handle<bg::Scalar<Key>>);
static_assert(equality_comparable_scalar_handle<bg::Scalar<Key>>);
static_assert(hashable_scalar_handle<bg::Scalar<bg::List<Key>>>);
static_assert(equality_comparable_scalar_handle<bg::Scalar<bg::List<Key>>>);
static_assert(!std::default_initializable<bg::List<bg::SCALAR>>);

} // namespace

TEST_CASE("a scalar value maps signed integers to int64", "[scalar]") {
    const auto value = bg::ScalarValue{42};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value->value() == 42);
}

TEST_CASE("a scalar value accepts lossless unsigned integer conversion", "[scalar]") {
    const auto value = bg::ScalarValue{42U};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<std::int64_t>>);
    CHECK(value->value() == 42);
}

TEST_CASE("a pointer-sized scalar value uses inline storage", "[scalar]") {
    const auto value = bg::ScalarValue{42, *std::pmr::null_memory_resource()};

    CHECK(value->value() == 42);
}

TEST_CASE("an evaluation time scalar value retains its graph type", "[scalar]") {
    const auto source = bg::MIN_DT + std::chrono::microseconds{42};
    const auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::EvaluationTime>>);
    CHECK(&value->type() == &bg::evaluation_time_type());
    CHECK(value->value() == source);
}

TEST_CASE("a scalar value owns character data", "[scalar]") {
    auto source = std::string{"value"};
    auto value = bg::ScalarValue{source};

    STATIC_CHECK(std::same_as<decltype(value), bg::ScalarValue<bg::string>>);
    source.assign("changed");

    CHECK(value->value() == "value");
}

TEST_CASE("a scalar value accepts a string literal", "[scalar]") {
    const auto value = bg::ScalarValue{"value"};

    STATIC_CHECK(std::same_as<decltype(value), const bg::ScalarValue<bg::string>>);
    CHECK(value->value() == "value");
}

TEST_CASE("a scalar value uses the supplied memory resource", "[scalar]") {
    class CountingResource final : public std::pmr::memory_resource {
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
    } memory;

    {
        auto value = bg::ScalarValue{std::string_view{"long enough to require separate character storage"}, memory};
        CHECK(value->value() == "long enough to require separate character storage");
    }

    CHECK(memory.allocations == memory.deallocations);
}

TEST_CASE("an opaque scalar value owns a copy", "[scalar]") {
    auto source = private_data{{1, 2, 3, 4}};
    const auto value = PrivateDataValue{source};

    source.values[0] = 5;

    CHECK(value->value().values == std::array{1, 2, 3, 4});
    CHECK(&value->type() == &bg::cpp_scalar_type<bg::Opaque<private_data>>());
}

TEST_CASE("a pointer-sized opaque scalar value uses inline storage", "[scalar]") {
    auto source = private_data{{1, 2, 3, 4}};
    const auto value = PrivatePointerValue{&source, *std::pmr::null_memory_resource()};

    CHECK(value->value() == &source);
}

TEST_CASE("a scalar value uses the current node memory resource", "[scalar]") {
    std::pmr::unsynchronized_pool_resource memory;
    const auto graph = 1;
    const auto context = bg::details::ScalarAllocationContext{memory, &graph, 2};
    const auto value = bg::ScalarValue{"value"};

    CHECK(value->value() == "value");
}

TEST_CASE("a scalar value transfers ownership when moved", "[scalar]") {
    auto source = bg::ScalarValue{std::string_view{"value"}};
    auto destination = std::move(source);

    CHECK(destination->value() == "value");
}

TEST_CASE("an erased scalar value copies a borrowed value", "[scalar]") {
    auto source = std::pmr::string{"value"};
    const auto view = bg::Scalar<bg::SCALAR>::from<bg::string>(source);
    auto value = bg::ScalarValue<>{view};

    source.assign("changed");

    const auto expected_value = std::pmr::string{"value"};
    CHECK(&value->type() == &bg::string_type());

    auto copy = std::pmr::string{};
    value->assign(&copy, bg::cpp_scalar_storage_representation<bg::string>());
    CHECK(copy == expected_value);
}

TEST_CASE("an erased scalar value can replace its value", "[scalar]") {
    const auto first = bg::ScalarValue{std::string_view{"first"}};
    const auto second = bg::ScalarValue{42};
    auto value = bg::ScalarValue<>{first};

    value = second;

    const auto view = bg::Scalar<>::from(value.view());
    CHECK(bg::scalar_cast<std::int64_t>(view)->value() == 42);
}

TEST_CASE("an erased scalar value can replace its value from a borrowed view", "[scalar]") {
    const auto source_value = std::int64_t{42};
    const auto source = bg::Scalar<std::int64_t>::from(source_value);
    const auto initial = bg::ScalarValue{std::string_view{"first"}};
    auto value = bg::ScalarValue<>{initial};

    value = source;

    const auto view = bg::Scalar<>::from(value.view());
    CHECK(bg::scalar_cast<std::int64_t>(view)->value() == source_value);
}

TEST_CASE("scalar handles compare by type and value", "[scalar]") {
    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto unequal_value = std::int64_t{43};
    const auto double_value = 42.0;
    const auto left = bg::Scalar<std::int64_t>::from(left_value);
    const auto equal = bg::Scalar<std::int64_t>::from(equal_value);
    const auto different_value = bg::Scalar<std::int64_t>::from(unequal_value);
    const auto different_type = bg::Scalar<double>::from(double_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different_value);
    CHECK_FALSE(left == different_type);
    CHECK(left->hash() == equal->hash());
}

TEST_CASE("a constrained scalar variable exposes its required operations", "[scalar]") {
    const auto left_value = std::int64_t{42};
    const auto equal_value = std::int64_t{42};
    const auto different_value = std::int64_t{43};
    const auto left = bg::Scalar<Key>::from<std::int64_t>(left_value);
    const auto equal = bg::Scalar<Key>::from<std::int64_t>(equal_value);
    const auto different = bg::Scalar<Key>::from<std::int64_t>(different_value);

    CHECK(left == equal);
    CHECK_FALSE(left == different);
    CHECK(left->hash() == equal->hash());
    CHECK(&left->type() == &bg::int64_type());
}

TEST_CASE("a scalar list owns its items", "[scalar]") {
    auto source = std::vector{1.0F, 2.0F};
    auto value = bg::ScalarValue<bg::List<float>>{source};

    source[0] = 3.0F;

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == 1.0F);
    CHECK(value->value()[1] == 2.0F);
    const auto &type = dynamic_cast<const bg::ListScalarType &>(value->type());
    CHECK(&type.item_type() == &bg::float_type());
}

TEST_CASE("a scalar list owns variable-size items", "[scalar]") {
    auto source_text = std::string{"first"};
    const auto source = std::vector<std::string_view>{source_text, "second"};
    const auto value = bg::ScalarValue<bg::List<bg::string>>{source};

    source_text.assign("changed");

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == "first");
    CHECK(value->value()[1] == "second");
}

TEST_CASE("a scalar list can contain scalar lists", "[scalar]") {
    const auto first = std::vector{1.0F, 2.0F};
    const auto second = std::vector{3.0F};
    const auto first_value = bg::ScalarValue<bg::List<float>>{first};
    const auto second_value = bg::ScalarValue<bg::List<float>>{second};
    const auto source = std::vector{first_value->value(), second_value->value()};
    const auto value = bg::ScalarValue<bg::List<bg::List<float>>>{source};

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == first_value->value());
    CHECK(value->value()[1] == second_value->value());
}

TEST_CASE("a scalar list can contain evaluation times", "[scalar]") {
    const auto source =
        std::vector{bg::MIN_DT + std::chrono::microseconds{2}, bg::MIN_DT + std::chrono::microseconds{5}};
    const auto value = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};
    const auto equal = bg::ScalarValue<bg::List<bg::EvaluationTime>>{source};

    REQUIRE(value->value().size() == 2);
    CHECK(value->value()[0] == source[0]);
    CHECK(value->value()[1] == source[1]);
    CHECK(value == equal);
    CHECK(value->hash() == equal->hash());
}
