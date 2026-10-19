#include <bg/graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/time_series/tick_serializer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

namespace {

template <template <typename, auto...> typename Field> struct OutputBundle {
    Field<bg::TS<bg::i64>> value;
    Field<bg::TSD<bg::i64, bg::TS<bg::i64>>> nested;
};

template <template <typename, auto...> typename Field> struct OutputScalarBundle {
    Field<bg::i64> value;
};

class ExtensionTimeSeriesType final : public bg::TimeSeriesType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "test_extension"; }
};

inline int extension_comparison_count{};
inline const literal::TimeSeriesLiteral *expected_extension_literal{};
inline bool extension_literal_address_matches{};

const ExtensionTimeSeriesType &extension_time_series_type() {
    static const auto type = bg::util::static_storage<ExtensionTimeSeriesType>{};
    [[maybe_unused]] static const auto &registered = bg::TypeRegistry::register_static(type.get());
    return type.get();
}

TEST_CASE("test_graph captures TSL and TSS output deltas",
          "[testing][evaluation][feature:test-graph][series:fixed-tsl][series:tss][transition:update]") {
    const auto list_identity = [](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> input) { return input; };
    const auto list = bg::testing::test_graph(
        list_identity, {{{0, 1}, {1, 10}}, {{1, 20}}, literal::INVALID, {{0, 3}}});

    const auto set_identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };
    const auto set = bg::testing::test_graph(set_identity, {{1, 2}, {literal::REMOVED(1), 3}, literal::SKIP});
    const auto empty_set = bg::testing::test_graph(set_identity, {{}});

    CHECK(list.equals({{{0, 1}, {1, 10}}, {{1, 20}}, literal::INVALID, {{0, 3}}}));
    CHECK(list.equals({{{0, 1}, {1, 10}}, {{0, literal::SKIP}, {1, 20}},
                       literal::INVALID, {{0, 3}}}));
    CHECK_FALSE(list.equals({{{0, literal::SKIP}, {1, 10}}, {{0, literal::SKIP}, {1, 20}},
                             literal::INVALID, {{0, 3}}}));
    CHECK_FALSE(list.equals({{{0, 1}, {1, 10}}, {{0, 20}}, literal::INVALID, {{0, 3}}}));
    CHECK_FALSE(list.equals({{{0, 1}, {1, 10}}, {{1, 20}}, literal::SKIP, {{0, 3}}}));
    CHECK(set.equals({{2, 1}, {3, literal::REMOVED(1)}, literal::SKIP}));
    CHECK_FALSE(set.equals({{1, 2}, {3, literal::REMOVED(2)}, literal::SKIP}));
    CHECK(empty_set.equals({{}}));
    CHECK(empty_set.equals({{literal::SKIP}}));
}

TEST_CASE("test_graph compares runtime-typed scalar bundle literals",
          "[testing][evaluation][feature:test-graph][series:value][scalar:hierarchical][declaration:generic]") {
    const auto identity = [](bg::wp<> input) { return bg::wp<>{input}; };
    const auto actual = bg::testing::test_graph(
        identity,
        bg::testing::input<bg::TS<bg::Bundle<OutputScalarBundle>>>({{{"value", 42}}}));

    CHECK(actual.equals({{{"value", 42}}}));
}

TEST_CASE("test_graph extension literal comparison delegates nested ticks to the framework",
          "[testing][evaluation][feature:test-graph][series:value]") {
    using Comparison = bg::testing::details::LiteralMatcher;
    using Dispatch = bg::TimeSeriesDispatch<bg::testing::TickLiteralComparison>;

    const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};
    auto child_state = bg::TimeSeriesState{};
    child_state.publish(time);
    const auto child = bg::i64{17};
    const auto child_input = bg::In<bg::TS<bg::i64>>{bg::TimeSeriesInputData{
        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>(), child_state, &child, true}};
    auto child_buffer = bg::util::buffer::contiguous(32);
    auto child_writer = bg::TimeSeriesTickWriter{child_buffer};
    REQUIRE(child_writer.write(child_input, time));

    auto buffer = bg::util::buffer::contiguous(64);
    auto writer = bg::util::buffer_writer{buffer};
    auto encoded = bg::ScalarBufferWriteContext{writer};
    bg::details::TickHeader{bg::details::TickOperation::tick, true}.write(encoded);
    writer.write_bytes(child_buffer.bytes());
    writer.commit();

    const auto operations = bg::testing::TickLiteralComparison{
        .compare = [](bg::util::buffer_reader &reader, const bg::TimeSeriesType &,
                      const literal::TimeSeriesLiteral &expected) {
            ++extension_comparison_count;
            if (expected_extension_literal != nullptr) {
                extension_literal_address_matches = &expected == expected_extension_literal;
                const auto comparison_value = bg::i64{17};
                const auto comparison_literal = literal::TimeSeriesLiteral(comparison_value);
                return extension_literal_address_matches &&
                       Comparison::matches_child(reader, bg::TS<bg::i64>::type(), comparison_literal);
            }
            return Comparison::matches_child(reader, bg::TS<bg::i64>::type(), expected);
        }};
    REQUIRE(Dispatch::instance().add(extension_time_series_type(), operations));

    const auto actual = std::optional{buffer};
    const auto expected_value = bg::i64{17};
    const auto expected = literal::TimeSeriesLiteral(expected_value);
    const auto mismatch_value = bg::i64{18};
    const auto mismatch = literal::TimeSeriesLiteral(mismatch_value);
    extension_comparison_count = 0;
    CHECK(Comparison::matches_tick(actual, &extension_time_series_type(), expected));
    CHECK_FALSE(Comparison::matches_tick(actual, &extension_time_series_type(), mismatch));
    CHECK(Comparison::matches_tick(actual, &extension_time_series_type(), expected));
    CHECK(extension_comparison_count == 3);

    static const auto same_name_sentinel = literal::Sentinel{literal::SKIP.name};
    const auto same_name_skip = literal::TimeSeriesLiteral(same_name_sentinel);
    REQUIRE(literal::Sentinel::from(same_name_skip) == &same_name_sentinel);
    const auto bare_remove = literal::TimeSeriesLiteral(literal::REMOVE);
    REQUIRE(literal::Sentinel::from(bare_remove) == &literal::REMOVE);
    const auto tagged_argument_value = bg::i64{17};
    const auto tagged_argument = literal::ScalarLiteral(tagged_argument_value);
    const auto removed_instruction = literal::REMOVED(tagged_argument);
    const auto tagged_removed = literal::TimeSeriesLiteral(removed_instruction);
    REQUIRE(literal::TaggedLiteral::from(tagged_removed).has_value());
    for (const auto *literal_value : {&same_name_skip, &bare_remove, &tagged_removed}) {
        expected_extension_literal = literal_value;
        extension_literal_address_matches = false;
        CHECK(Comparison::matches_tick(actual, &extension_time_series_type(), *literal_value));
        CHECK(extension_literal_address_matches);
    }
    CHECK(extension_comparison_count == 6);
    expected_extension_literal = nullptr;

    auto invalid_buffer = bg::util::buffer::contiguous(8);
    auto invalid_writer = bg::util::buffer_writer{invalid_buffer};
    auto invalid_encoded = bg::ScalarBufferWriteContext{invalid_writer};
    bg::details::TickHeader{bg::details::TickOperation::tick, false}.write(invalid_encoded);
    invalid_writer.commit();
    CHECK(Comparison::matches_tick(std::optional{invalid_buffer}, &extension_time_series_type(), literal::INVALID));
    CHECK(extension_comparison_count == 6);
}

TEST_CASE("test_graph preserves missing and empty collection output ticks",
          "[testing][evaluation][feature:test-graph][series:tss][transition:first-tick]") {
    const auto identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };

    const auto skipped = bg::testing::test_graph(
        identity, {literal::SKIP, literal::SKIP, literal::SKIP});
    const auto no_ticks = bg::testing::test_graph(
        identity, bg::testing::TestGraphLiteralInput(std::span<const literal::TimeSeriesLiteral>{}));
    const auto empty = bg::testing::test_graph(identity, {{}});

    CHECK(skipped.equals({literal::SKIP, literal::SKIP, literal::SKIP}));
    CHECK(no_ticks.equals({literal::SKIP}));
    CHECK(empty.equals({{}}));
}

TEST_CASE("test_graph result visits captured ticks without consuming owning scalar values",
          "[testing][evaluation][feature:test-graph][series:value][scalar:string][feature:scalar-storage]") {
    const auto identity = [](bg::wp<bg::TS<bg::string>> input) { return input; };
    const auto result = bg::testing::test_graph(identity, {"first", literal::SKIP, literal::INVALID, "last"});

    const auto read_value = [&](std::size_t index) {
        auto value = std::string{};
        const auto visited = result.visit_tick(index, [&](const bg::TimeSeriesType &type, bool valid,
                                                          bg::util::buffer_reader &reader) {
            REQUIRE(valid);
            const auto &value_type = bg::type_cast<bg::TimeSeriesValueType>(type);
            REQUIRE(bg::details::TimeSeriesTickScalarRead::visit(reader, value_type.value_type(),
                                                                  [&](bg::Scalar<> scalar) {
                                                                      value = bg::scalar_cast<bg::string>(scalar)
                                                                                  .value();
                                                                  }));
        });
        REQUIRE(visited);
        return value;
    };

    CHECK(read_value(0) == "first");
    CHECK_THROWS(result.visit_tick(0, [&](const bg::TimeSeriesType &type, bool valid,
                                          bg::util::buffer_reader &reader) {
        REQUIRE(valid);
        const auto &value_type = bg::type_cast<bg::TimeSeriesValueType>(type);
        REQUIRE(bg::details::TimeSeriesTickScalarRead::visit(reader, value_type.value_type(),
                                                              [](bg::Scalar<> scalar) {
                                                                  CHECK(bg::scalar_cast<bg::string>(scalar).value() ==
                                                                        "first");
                                                              }));
        throw std::runtime_error{"callback"};
    }));
    CHECK(read_value(0) == "first");

    auto skipped_callback_called = false;
    CHECK_FALSE(result.visit_tick(1, [&](const auto &, bool, auto &) { skipped_callback_called = true; }));
    CHECK_FALSE(skipped_callback_called);

    auto invalid_callback_called = false;
    CHECK(result.visit_tick(2, [&](const auto &, bool valid, auto &) {
        invalid_callback_called = true;
        CHECK_FALSE(valid);
    }));
    CHECK(invalid_callback_called);
    CHECK(read_value(3) == "last");
    CHECK(result[0]->view().value() == "first");
}

TEST_CASE("test_graph captured tick chain rolls over and remains readable after comparisons and moves",
          "[testing][evaluation][feature:test-graph][series:tsd][scalar:string][feature:scalar-storage]") {
    using Tick = literal::TimeSeriesLiteral;
    struct EntryStorage {
        std::string key;
        std::string value;
        std::array<Tick, 2> literal;
    };
    constexpr auto entry_count = std::size_t{14};

    const auto make_entries = [](bool mismatch) {
        auto pairs = std::vector<EntryStorage>{};
        pairs.reserve(entry_count);
        auto entries = std::vector<Tick>{};
        entries.reserve(entry_count);
        for (auto index = std::size_t{}; index < entry_count; ++index) {
            auto key = std::string{"key-"} + std::to_string(index);
            auto value = std::string{"value-"} + std::to_string(index);
            if (mismatch && index == 0) {
                value = "different";
            }
            pairs.push_back(EntryStorage{std::move(key), std::move(value), {}});
            auto &pair = pairs.back();
            pair.literal = {Tick(pair.key), Tick(pair.value)};
            entries.emplace_back(std::span<const Tick>{pair.literal});
        }
        return std::pair{std::move(pairs), std::move(entries)};
    };

    auto expected_storage = make_entries(false);
    const auto expected_tick = Tick(std::span<const Tick>{expected_storage.second});
    const auto input_ticks = std::array{expected_tick};
    const auto identity = [](bg::wp<bg::TSD<bg::string, bg::TS<bg::string>>> input) { return input; };
    auto result = bg::testing::test_graph(
        identity, bg::testing::TestGraphLiteralInput(std::span<const Tick>{input_ticks}));

    auto mismatch_storage = make_entries(true);
    const auto mismatch_tick = Tick(std::span<const Tick>{mismatch_storage.second});
    CHECK(result.equals({expected_tick}));
    CHECK_FALSE(result.equals({mismatch_tick}));
    CHECK(result.equals({expected_tick}));

    auto moved_result = std::move(result);
    CHECK(moved_result.equals({expected_tick}));
}

TEST_CASE("test_graph captures owning scalar strings across cycles and result lifetimes",
          "[testing][evaluation][feature:test-graph][series:value][scalar:string][feature:scalar-storage]") {
    auto ticks = std::vector<std::string>{};
    for (auto index = std::size_t{}; index < 20; ++index) {
        ticks.emplace_back(24, static_cast<char>('a' + index));
    }
    const auto identity = [](bg::wp<bg::TS<bg::string>> input) { return input; };

    auto retained = [&] {
        auto result = bg::testing::test_graph(identity, ticks);
        for (auto index = std::size_t{}; index < ticks.size(); ++index) {
            const auto value = result[index];
            CHECK(value.has_value());
            if (value) {
                CHECK(value->view().value() == ticks[index]);
            }
        }
        return result[17];
    }();

    CHECK(retained->view().value() == ticks[17]);
}

TEST_CASE("test_graph captures nested TSD and TSB output deltas",
          "[testing][evaluation][feature:test-graph][series:tsd][series:tsb][shape:nested]") {
    const auto bundle_identity = [](bg::wp<bg::TSB<OutputBundle>> input) { return input; };
    const auto bundle = bg::testing::test_graph(
        bundle_identity,
        {{{"value", 1}, {"nested", {{2, 20}, {3, 30}}}},
         {{"value", 4}, {"nested", {{3, 31}}}},
         literal::SKIP});

    const auto nested_identity = [](bg::wp<bg::TSD<bg::i64, bg::TSB<OutputBundle>>> input) { return input; };
    const auto nested = bg::testing::test_graph(
        nested_identity,
        {{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
         {{1, {{"nested", {{2, literal::REMOVE}}}}}},
         {{1, literal::REMOVE}}});
    const auto dictionary_identity = [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto dictionary = bg::testing::test_graph(dictionary_identity, {{{1, 10}, {2, 20}}});

    CHECK(bundle.equals({{{"value", 1}, {"nested", {{2, 20}, {3, 30}}}},
                         {{"value", 4}, {"nested", {{3, 31}}}}, literal::SKIP}));
    CHECK(nested.equals({{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
                         {{1, {{"nested", {{2, literal::REMOVE}}}}}},
                         {{1, literal::REMOVE}}}));
    CHECK_FALSE(nested.equals({{{1, {{"value", 1}, {"nested", {{2, 20}}}}}},
                               {{1, {{"nested", {{2, literal::REMOVE}}}}}},
                               {{2, literal::REMOVE}}}));
    CHECK(dictionary.equals({{{2, 20}, {1, 10}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}, {1, 10}, {2, 20}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}}));
}

TEST_CASE("test_graph compares invalid nested collection values",
          "[testing][evaluation][feature:test-graph][series:tsd][shape:nested][transition:invalidation]") {
    const auto invalidate_set = bg::node([](bg::Out<bg::TSD<bg::i64, bg::TSS<bg::i64>>> output) {
        output[1].invalidate();
    });
    const auto invalidate_dictionary = bg::node(
        [](bg::Out<bg::TSD<bg::i64, bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) { output[1].invalidate(); });
    const auto set_child_graph = [&] { return invalidate_set(); };
    const auto dictionary_child_graph = [&] { return invalidate_dictionary(); };
    const auto set_child = bg::testing::test_graph(set_child_graph);
    const auto dictionary_child = bg::testing::test_graph(dictionary_child_graph);
    const auto scalar_child_identity =
        [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto scalar_child = bg::testing::test_graph(scalar_child_identity, {{{1, literal::INVALID}}});

    CHECK(set_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(set_child.equals({{{1, {}}}}));
    CHECK(dictionary_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(dictionary_child.equals({{{1, {}}}}));
    CHECK(scalar_child.equals({{{1, literal::INVALID}}}));
    CHECK_FALSE(scalar_child.equals({{{1, 1}}}));
}

TEST_CASE("test_graph captures a top-level reference to a collection",
          "[testing][evaluation][feature:test-graph][feature:reference][series:tsd]"
          "[binding:reference-to-value][transition:update]") {
    const auto select = bg::node([](bg::In<bg::TS<bool>> choose_second,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> first,
                                    bg::In<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> second,
                                    bg::Out<bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>> output) {
        output.copy(*choose_second ? second : first);
    });
    const auto graph = [&](bg::wp<bg::TS<bool>> choose_second,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> first,
                           bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> second) {
        return select(choose_second, first, second);
    };
    const auto actual = bg::testing::test_graph(graph, {false, true}, {{{1, 10}}}, {{{2, 20}}});

    CHECK(actual.equals({{{1, 10}}, {{1, literal::REMOVE}, {2, 20}}}));
}

TEST_CASE("test_graph rejects nested references in collection output capture",
          "[testing][evaluation][feature:test-graph][feature:reference][series:tsd][series:tss]"
          "[binding:reference-to-value][shape:nested]") {
    const auto make_dictionary = bg::node([](bg::In<bg::REF<bg::TSS<bg::i64>>> item,
                                             bg::Out<bg::TSD<bg::i64, bg::REF<bg::TSS<bg::i64>>>> output) {
        output[1].copy(item);
    });
    const auto graph = [&](bg::wp<bg::TSS<bg::i64>> input) { return make_dictionary(input); };

    CHECK_THROWS_AS(bg::testing::test_graph(graph, {{1, 2}}), std::invalid_argument);
}

TEST_CASE("test_graph owns string keys and values in captured collection deltas",
          "[testing][evaluation][feature:test-graph][series:tsd][scalar:string][feature:scalar-storage]") {
    const auto actual = [] {
        const auto key = std::string(128, 'k');
        const auto first_value = std::string(160, 'a');
        const auto replacement_value = std::string(192, 'b');
        const auto identity = [](bg::wp<bg::TSD<bg::string, bg::TS<bg::string>>> input) { return input; };
        return bg::testing::test_graph(identity, {{{key, first_value}}, {{key, replacement_value}}});
    }();

    const auto key = std::string(128, 'k');
    const auto first_value = std::string(160, 'a');
    const auto replacement_value = std::string(192, 'b');
    CHECK(actual.equals({{{key, first_value}}, {{key, replacement_value}}}));
}

TEST_CASE("test_graph rejects duplicate and extra expected collection removals",
          "[testing][evaluation][feature:test-graph][series:tss][series:tsd][transition:removal]") {
    const auto set_identity = [](bg::wp<bg::TSS<bg::i64>> input) { return input; };
    const auto set = bg::testing::test_graph(set_identity, {{1}, {literal::REMOVED(1)}});
    const auto dictionary_identity =
        [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto dictionary = bg::testing::test_graph(dictionary_identity, {{{1, 10}}, {{1, literal::REMOVE}}});

    CHECK(set.equals({{1}, {literal::REMOVED(1)}}));
    CHECK_FALSE(set.equals({{1}, {literal::REMOVED(1), literal::REMOVED(1)}}));
    CHECK_FALSE(set.equals({{1}, {literal::REMOVED(1), literal::REMOVED(2)}}));
    CHECK(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}, {1, literal::REMOVE}}}));
    CHECK_FALSE(dictionary.equals({{{1, 10}}, {{1, literal::REMOVE}, {2, literal::REMOVE}}}));
}

TEST_CASE("test_graph scalar result access rejects collection ticks",
          "[testing][evaluation][feature:test-graph][series:tsd]") {
    const auto identity = [](bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) { return input; };
    const auto actual = bg::testing::test_graph(identity, {{{1, 10}}});

    CHECK_THROWS_AS(actual[0], std::invalid_argument);
}

} // namespace
