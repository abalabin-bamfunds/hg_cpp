#include <bg/scalar/set/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("a scalar set owns its unique items", "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto items = std::vector<std::int64_t>{3, 1, 3, 2};
    const auto value = bg::ScalarValue<bg::Set<std::int64_t>>{items};
    const auto set = value.view();

    CHECK(set.size() == 3);
    CHECK(set.contains(1));
    CHECK(set.contains(2));
    CHECK(set.contains(3));
    CHECK_FALSE(set.contains(4));

    auto sum = std::int64_t{};
    set.for_each([&](const auto item) { sum += item.value(); });
    CHECK(sum == 6);
}

TEST_CASE("a scalar set accepts its exact non-canonical item type",
          "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto value = bg::ScalarValue<bg::Set<int>>{std::vector{1, 2}};
    const auto set = value.view();

    CHECK(set.contains(1));
    CHECK_FALSE(set.contains(3));
}

TEST_CASE("scalar set equality is independent of insertion order",
          "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto first = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{1, 2, 3}};
    const auto second = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{3, 1, 2}};
    const auto different = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{1, 2, 4}};

    CHECK(first == second);
    CHECK_FALSE(first == different);
    CHECK(first.view().hash() == second.view().hash());
}

TEST_CASE("a scalar set owns variable-size items",
          "[value][scalar][set][scalar:set][feature:scalar-collection][storage:variable]") {
    auto source = std::string{"first"};
    const auto value = bg::ScalarValue<bg::Set<bg::string>>{std::vector<std::string_view>{source, "second"}};

    source.assign("changed");

    const auto set = value.view();
    CHECK(set.contains("first"));
    CHECK(set.contains("second"));
    CHECK_FALSE(set.contains("changed"));
}

TEST_CASE("a generic scalar set exposes its declared comparison operations",
          "[value][scalar][set][scalar:set][generic][declaration:generic][feature:scalar-collection]"
          "[scalar:generic]") {
    using Item = bg::ScalarVariable<"Item", bg::EqualityComparable, bg::Hashable>;

    const auto first = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{1, 2}};
    const auto second = bg::ScalarValue<bg::Set<std::int64_t>>{std::vector<std::int64_t>{2, 1}};
    const auto left = bg::Scalar<bg::Set<Item>>::from(first);
    const auto right = bg::Scalar<bg::Set<Item>>::from(second);

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}
