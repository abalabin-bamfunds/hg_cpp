#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_CASE("a scalar dictionary owns and exposes its entries",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto items = std::vector<std::pair<std::string_view, std::int64_t>>{{"one", 1}, {"two", 2}};
    const auto value = bg::ScalarValue<bg::Dict<bg::string, std::int64_t>>{items};
    const auto dict = value.view();

    CHECK(dict.size() == 2);
    CHECK(dict.contains("one"));
    CHECK(dict.at("two").value() == 2);
    CHECK_THROWS_AS(dict.at("three"), std::out_of_range);

    auto sum = std::int64_t{};
    dict.for_each([&](const auto, const auto item) { sum += item.value(); });
    CHECK(sum == 3);
}

TEST_CASE("scalar dictionary equality is independent of insertion order",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    using Entry = std::pair<std::int64_t, std::string_view>;
    using Value = bg::ScalarValue<bg::Dict<std::int64_t, bg::string>>;

    const auto first = Value{std::vector<Entry>{{1, "one"}, {2, "two"}}};
    const auto second = Value{std::vector<Entry>{{2, "two"}, {1, "one"}}};
    const auto different = Value{std::vector<Entry>{{1, "one"}, {2, "changed"}}};

    CHECK(first == second);
    CHECK_FALSE(first == different);
    CHECK(first.view().hash() == second.view().hash());
}

TEST_CASE("a scalar dictionary owns variable-size keys and values",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection][storage:variable]") {
    auto key = std::string{"key"};
    auto item = std::string{"value"};
    const auto value = bg::ScalarValue<bg::Dict<bg::string, bg::string>>{
        std::vector<std::pair<std::string_view, std::string_view>>{{key, item}}};

    key.assign("changed key");
    item.assign("changed value");

    const auto dict = value.view();
    CHECK(dict.contains("key"));
    CHECK(dict.at("key").value() == "value");
}

TEST_CASE("a scalar dictionary owns nested collection values",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto first = bg::ScalarValue<bg::List<std::int64_t>>{std::vector<std::int64_t>{1, 2}};
    const auto second = bg::ScalarValue<bg::List<std::int64_t>>{std::vector<std::int64_t>{3}};
    const auto items = std::vector<std::pair<std::string_view, bg::Scalar<bg::List<std::int64_t>>>>{
        {"first", first.view()}, {"second", second.view()}};

    const auto value = bg::ScalarValue<bg::Dict<bg::string, bg::List<std::int64_t>>>{items};
    const auto dict = value.view();

    REQUIRE(dict.at("first").size() == 2);
    CHECK(dict.at("first")[1].value() == 2);
    CHECK(dict.at("second")[0].value() == 3);
}

TEST_CASE("a generic scalar dictionary exposes its declared comparison operations",
          "[value][scalar][dict][scalar:dict][generic][declaration:generic][feature:scalar-collection]"
          "[scalar:generic]") {
    using Item = bg::ScalarVariable<"Item", bg::EqualityComparable, bg::Hashable>;
    using Entry = std::pair<std::int64_t, std::int64_t>;

    const auto first = bg::ScalarValue<bg::Dict<std::int64_t, std::int64_t>>{std::vector<Entry>{{1, 2}, {3, 4}}};
    const auto second = bg::ScalarValue<bg::Dict<std::int64_t, std::int64_t>>{std::vector<Entry>{{3, 4}, {1, 2}}};
    const auto left = bg::Scalar<bg::Dict<Item, Item>>::from(first.input_view());
    const auto right = bg::Scalar<bg::Dict<Item, Item>>::from(second.input_view());

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}
