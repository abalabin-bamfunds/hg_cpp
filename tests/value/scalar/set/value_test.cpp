#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace bg;

namespace {

template <template <typename, auto...> typename Field> struct SetItem {
    Field<i64> value;
};

} // namespace

TEST_CASE("a scalar set owns its unique items", "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto items = std::vector<i64>{3, 1, 3, 2};
    const auto value = ScalarValue<Set<i64>>{items};
    const auto set = value.view();

    CHECK(set.size() == 3);
    CHECK(set.contains(1));
    CHECK(set.contains(2));
    CHECK(set.contains(3));
    CHECK_FALSE(set.contains(4));

    auto sum = i64{};
    set.for_each([&](const auto item) { sum += item.value(); });
    CHECK(sum == 6);
}

TEST_CASE("a scalar set accepts its exact non-canonical item type",
          "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto value = ScalarValue<Set<int>>{std::vector{1, 2}};
    const auto set = value.view();

    CHECK(set.contains(1));
    CHECK_FALSE(set.contains(3));
}

TEST_CASE("scalar set equality is independent of insertion order",
          "[value][scalar][set][scalar:set][feature:scalar-collection]") {
    const auto first = ScalarValue<Set<i64>>{std::vector<i64>{1, 2, 3}};
    const auto second = ScalarValue<Set<i64>>{std::vector<i64>{3, 1, 2}};
    const auto different = ScalarValue<Set<i64>>{std::vector<i64>{1, 2, 4}};

    CHECK(first == second);
    CHECK_FALSE(first == different);
    CHECK(first.view().hash() == second.view().hash());
}

TEST_CASE("a scalar set owns variable-size items",
          "[value][scalar][set][scalar:set][feature:scalar-collection][storage:variable]") {
    auto source = std::string{"first"};
    const auto value = ScalarValue<Set<string>>{std::vector<std::string_view>{source, "second"}};

    source.assign("changed");

    const auto set = value.view();
    CHECK(set.contains("first"));
    CHECK(set.contains("second"));
    CHECK_FALSE(set.contains("changed"));
}

TEST_CASE("a scalar set owns and compares hierarchical items",
          "[value][scalar][set][scalar:set][scalar:hierarchical][feature:scalar-collection]") {
    auto allocator = bg::allocator{};
    const auto one = bundle<SetItem>(allocator, i64{1});
    const auto another_one = bundle<SetItem>(allocator, i64{1});
    const auto two = bundle<SetItem>(allocator, i64{2});
    const auto first_items = std::array<Scalar<Bundle<SetItem>>, 3>{one.view(), another_one.view(), two.view()};
    const auto second_items = std::array<Scalar<Bundle<SetItem>>, 2>{two.view(), one.view()};
    const auto first = ScalarValue<Set<Bundle<SetItem>>>{first_items};
    const auto second = ScalarValue<Set<Bundle<SetItem>>>{second_items};

    CHECK(first.view().size() == 2);
    CHECK(first.view().contains(one.view()));
    CHECK(first.view().contains(two.view()));
    CHECK(first == second);
}

TEST_CASE("a generic scalar set exposes its declared comparison operations",
          "[value][scalar][set][scalar:set][generic][declaration:generic][feature:scalar-collection]"
          "[scalar:generic]") {
    using ITEM = ScalarVariable<"ITEM", EqualityComparable, Hashable>;

    const auto first = ScalarValue<Set<i64>>{std::vector<i64>{1, 2}};
    const auto second = ScalarValue<Set<i64>>{std::vector<i64>{2, 1}};
    const auto left = Scalar<Set<ITEM>>::from(first);
    const auto right = Scalar<Set<ITEM>>::from(second);

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}
