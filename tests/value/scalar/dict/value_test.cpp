#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bg;

TEST_CASE("a scalar dictionary owns and exposes its entries",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto items = std::vector<std::pair<std::string_view, i64>>{{"one", 1}, {"two", 2}};
    const auto value = ScalarValue<Dict<string, i64>>{items};
    const auto dict = value.view();

    CHECK(dict.size() == 2);
    CHECK(dict.contains("one"));
    CHECK(dict.at("two").value() == 2);
    CHECK_THROWS_AS(dict.at("three"), std::out_of_range);

    auto sum = i64{};
    dict.for_each([&](const auto, const auto item) { sum += item.value(); });
    CHECK(sum == 3);
}

TEST_CASE("a scalar dictionary finds mapped values with one lookup",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto value = ScalarValue<Dict<i64, i64>>{std::vector<std::pair<i64, i64>>{{1, 10}, {2, 20}}};
    const auto dict = value.view();

    const auto found = dict.find(2);
    REQUIRE(found.has_value());
    CHECK(found->value() == 20);
    CHECK_FALSE(dict.find(3).has_value());

    const auto key = ScalarValue{1};
    const auto found_by_scalar = dict.find(key.view());
    REQUIRE(found_by_scalar.has_value());
    CHECK(found_by_scalar->value() == 10);
}

TEST_CASE("a scalar dictionary cursor fills caller-owned batches",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto value =
        ScalarValue<Dict<i64, i64>>{std::vector<std::pair<i64, i64>>{{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}}};
    const auto dict = value.view();
    auto cursor = dict.make_cursor();
    auto buffer = std::array<ScalarDictRuntimeType::BatchEntry, 2>{};
    auto entry_count = std::size_t{};
    auto batch_count = std::size_t{};
    auto key_sum = i64{};
    auto mapped_sum = i64{};

    while (const auto count = cursor->next_batch(buffer)) {
        ++batch_count;
        entry_count += count;
        for (const auto &entry : std::span{buffer}.first(count)) {
            key_sum += Scalar<i64>::from(dict.runtime_type().key_runtime_type(), entry.key).value();
            mapped_sum += Scalar<i64>::from(dict.runtime_type().mapped_runtime_type(), entry.mapped).value();
        }
    }

    CHECK(entry_count == 5);
    CHECK(batch_count == 3);
    CHECK(key_sum == 15);
    CHECK(mapped_sum == 15);
    CHECK(cursor->next_batch(buffer) == 0);
}

TEST_CASE("a scalar dictionary accepts its exact non-canonical key type",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto value = ScalarValue<Dict<int, int>>{std::vector<std::pair<int, int>>{{1, 2}}};
    const auto dict = value.view();

    CHECK(dict.contains(1));
    CHECK(*dict.at(1) == 2);
}

TEST_CASE("scalar dictionary equality is independent of insertion order",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    using Entry = std::pair<i64, std::string_view>;
    using Value = ScalarValue<Dict<i64, string>>;

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
    const auto value =
        ScalarValue<Dict<string, string>>{std::vector<std::pair<std::string_view, std::string_view>>{{key, item}}};

    key.assign("changed key");
    item.assign("changed value");

    const auto dict = value.view();
    CHECK(dict.contains("key"));
    CHECK(dict.at("key").value() == "value");
}

TEST_CASE("a scalar dictionary owns nested collection values",
          "[value][scalar][dict][scalar:dict][feature:scalar-collection]") {
    const auto first = ScalarValue<List<i64>>{std::vector<i64>{1, 2}};
    const auto second = ScalarValue<List<i64>>{std::vector<i64>{3}};
    const auto items =
        std::vector<std::pair<std::string_view, Scalar<List<i64>>>>{{"first", first.view()}, {"second", second.view()}};

    const auto value = ScalarValue<Dict<string, List<i64>>>{items};
    const auto dict = value.view();

    REQUIRE(dict.at("first").size() == 2);
    CHECK(dict.at("first")[1].value() == 2);
    CHECK(dict.at("second")[0].value() == 3);
}

TEST_CASE("a generic scalar dictionary exposes its declared comparison operations",
          "[value][scalar][dict][scalar:dict][generic][declaration:generic][feature:scalar-collection]"
          "[scalar:generic]") {
    using ITEM = ScalarVariable<"ITEM", EqualityComparable, Hashable>;
    using Entry = std::pair<i64, i64>;

    const auto first = ScalarValue<Dict<i64, i64>>{std::vector<Entry>{{1, 2}, {3, 4}}};
    const auto second = ScalarValue<Dict<i64, i64>>{std::vector<Entry>{{3, 4}, {1, 2}}};
    const auto left = Scalar<Dict<ITEM, ITEM>>::from(first);
    const auto right = Scalar<Dict<ITEM, ITEM>>::from(second);

    CHECK(left == right);
    CHECK(left == second);
    CHECK(second == left);
    CHECK(left.hash() == right.hash());
}
