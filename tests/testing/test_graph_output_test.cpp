#include <bg/graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/time_series/tick_serializer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
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

class StatefulExtensionTimeSeriesType final : public bg::TimeSeriesType {
  public:
    StatefulExtensionTimeSeriesType() : TimeSeriesType{bg::TimeSeriesGuarantee::ordinary} {}

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "test_stateful_extension"; }
};

struct StatefulComparisonStats {
    std::size_t created{};
    std::size_t compared{};
    std::size_t finished{};
    std::vector<std::size_t> compared_at;
    std::vector<std::size_t> invalidated;
};

inline StatefulComparisonStats stateful_comparison_stats;

class StatefulExtensionComparison final : public bg::testing::TickLiteralComparisonState {
  public:
    bool compare(bg::util::buffer_reader &reader, const bg::TimeSeriesType &,
                 const literal::TimeSeriesLiteral &expected, std::size_t tick_index) override {
        ++stateful_comparison_stats.compared;
        stateful_comparison_stats.compared_at.push_back(tick_index);
        const auto actual = reader.read_raw<bg::i64>();
        const auto wanted = bg::ScalarValue<bg::i64>::from_literal(expected);
        if (actual != wanted.view().value() || (has_previous_ && actual != previous_ + 1)) {
            return false;
        }

        has_previous_ = true;
        previous_ = actual;
        can_finish_ = actual >= 0;
        return true;
    }

    void invalidate(std::size_t tick_index) override { stateful_comparison_stats.invalidated.push_back(tick_index); }

    bool finish() override {
        ++stateful_comparison_stats.finished;
        return can_finish_;
    }

  private:
    bg::i64 previous_{};
    bool has_previous_{};
    bool can_finish_{true};
};

struct DictionaryEntry {
    bg::details::TickOperation operation;
    bg::i64 key;
    bg::i64 value{};
};

const StatefulExtensionTimeSeriesType &stateful_extension_time_series_type() {
    static const auto type = bg::util::static_storage<StatefulExtensionTimeSeriesType>{};
    [[maybe_unused]] static const auto &registered = bg::TypeRegistry::register_static(type.get());
    return type.get();
}

bg::util::buffer stateful_extension_tick(bg::i64 value, bool valid = true) {
    auto buffer = bg::util::buffer::contiguous(32);
    auto writer = bg::util::buffer_writer{buffer};
    auto encoded = bg::ScalarBufferWriteContext{writer};
    bg::details::TickHeader{bg::details::TickOperation::tick, valid}.write(encoded);
    if (valid) {
        bg::details::write_bytes(encoded, value);
    }
    writer.commit();
    return buffer;
}

bg::util::buffer dictionary_tick(std::initializer_list<DictionaryEntry> entries) {
    auto buffer = bg::util::buffer::contiguous(128);
    auto writer = bg::util::buffer_writer{buffer};
    auto encoded = bg::ScalarBufferWriteContext{writer};
    bg::details::TickHeader{bg::details::TickOperation::tick, true}.write(encoded);
    for (const auto &entry : entries) {
        const auto has_value = entry.operation != bg::details::TickOperation::remove;
        bg::details::TickHeader{entry.operation, has_value}.write(encoded);
        bg::details::write_bytes(encoded, entry.key);
        if (has_value) {
            bg::details::write_bytes(encoded, entry.value);
        }
    }
    bg::details::TickHeader{bg::details::TickOperation::end}.write(encoded);
    writer.commit();
    return buffer;
}

bg::util::buffer list_tick(bg::item_id_t index, bg::i64 value) {
    auto buffer = bg::util::buffer::contiguous(64);
    auto writer = bg::util::buffer_writer{buffer};
    auto encoded = bg::ScalarBufferWriteContext{writer};
    bg::details::TickHeader{bg::details::TickOperation::tick, true}.write(encoded);
    bg::details::TickHeader{bg::details::TickOperation::tick, true}.write(encoded);
    bg::details::write_bytes(encoded, index);
    bg::details::write_bytes(encoded, value);
    bg::details::TickHeader{bg::details::TickOperation::end}.write(encoded);
    writer.commit();
    return buffer;
}

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

TEST_CASE("test_graph stateful extension comparison spans cycles and nested lifetimes",
          "[testing][evaluation][feature:test-graph][series:generic]") {
    using Comparison = bg::testing::details::LiteralMatcher;
    using Dispatch = bg::TimeSeriesDispatch<bg::testing::TickLiteralComparison>;
    const auto &type = stateful_extension_time_series_type();

    const auto operations = bg::testing::TickLiteralComparison{
        .create_state = [] {
            ++stateful_comparison_stats.created;
            return std::make_unique<StatefulExtensionComparison>();
        }};
    REQUIRE(Dispatch::instance().add(type, operations));

    stateful_comparison_stats = {};
    const auto valid_tick = stateful_extension_tick(17);
    const auto invalid_tick = stateful_extension_tick(0, false);
    const std::array actual{std::optional{valid_tick}, std::optional<bg::util::buffer>{},
                            std::optional{invalid_tick}, std::optional{stateful_extension_tick(18)}};
    const bg::i64 expected_first{17};
    const bg::i64 expected_last{18};
    const std::array expected{literal::TimeSeriesLiteral(expected_first), literal::TimeSeriesLiteral(literal::SKIP),
                              literal::TimeSeriesLiteral(literal::INVALID),
                              literal::TimeSeriesLiteral(expected_last)};
    CHECK(Comparison::matches_tick(actual[1], &type, expected[1]));
    CHECK(Comparison::matches_tick(actual[2], &type, expected[2]));
    CHECK(Comparison::matches_sequence(actual, &type, expected));
    CHECK(stateful_comparison_stats.created == 1);
    CHECK(stateful_comparison_stats.compared == 2);
    CHECK(stateful_comparison_stats.compared_at == std::vector<std::size_t>{0, 3});
    CHECK(stateful_comparison_stats.invalidated == std::vector<std::size_t>{2});
    CHECK(stateful_comparison_stats.finished == 1);

    const std::array consecutive_actual{std::optional{stateful_extension_tick(20)},
                                        std::optional{stateful_extension_tick(21)}};
    const bg::i64 expected_twenty{20};
    const bg::i64 expected_twenty_one{21};
    const bg::i64 expected_twenty_two{22};
    const std::array consecutive_expected{literal::TimeSeriesLiteral(expected_twenty),
                                          literal::TimeSeriesLiteral(expected_twenty_one)};
    const auto created_before_repeated_comparisons = stateful_comparison_stats.created;
    CHECK(Comparison::matches_sequence(consecutive_actual, &type, consecutive_expected));
    const std::array mismatched_expected{literal::TimeSeriesLiteral(expected_twenty),
                                         literal::TimeSeriesLiteral(expected_twenty_two)};
    CHECK_FALSE(Comparison::matches_sequence(consecutive_actual, &type, mismatched_expected));
    CHECK(Comparison::matches_sequence(consecutive_actual, &type, consecutive_expected));
    CHECK(stateful_comparison_stats.created == created_before_repeated_comparisons + 3);

    const std::array negative_actual{std::optional{stateful_extension_tick(-1)}};
    const bg::i64 expected_negative{-1};
    const std::array negative_expected{literal::TimeSeriesLiteral(expected_negative)};
    CHECK_FALSE(Comparison::matches_sequence(negative_actual, &type, negative_expected));

    const auto &dictionary_type = bg::time_series_dictionary_type(bg::int64_type(), type,
                                                                  bg::TimeSeriesGuarantee::ordinary);
    const std::array dictionary_actual{
        std::optional{dictionary_tick({{bg::details::TickOperation::add, 1, 10},
                                       {bg::details::TickOperation::add, 2, 30}})},
        std::optional{dictionary_tick({{bg::details::TickOperation::tick, 1, 11},
                                       {bg::details::TickOperation::tick, 2, 31}})}};
    const bg::i64 key_one{1};
    const bg::i64 key_two{2};
    const bg::i64 value_ten{10};
    const bg::i64 value_thirty{30};
    const bg::i64 value_eleven{11};
    const bg::i64 value_thirty_one{31};
    const std::array first_key_one{literal::TimeSeriesLiteral(key_one), literal::TimeSeriesLiteral(value_ten)};
    const std::array first_key_two{literal::TimeSeriesLiteral(key_two), literal::TimeSeriesLiteral(value_thirty)};
    const std::array second_key_one{literal::TimeSeriesLiteral(key_one), literal::TimeSeriesLiteral(value_eleven)};
    const std::array second_key_two{literal::TimeSeriesLiteral(key_two), literal::TimeSeriesLiteral(value_thirty_one)};
    const std::array first_entries{literal::TimeSeriesLiteral(std::span{first_key_one}),
                                   literal::TimeSeriesLiteral(std::span{first_key_two})};
    const std::array second_entries{literal::TimeSeriesLiteral(std::span{second_key_one}),
                                    literal::TimeSeriesLiteral(std::span{second_key_two})};
    const std::array dictionary_expected{literal::TimeSeriesLiteral(std::span{first_entries}),
                                         literal::TimeSeriesLiteral(std::span{second_entries})};
    CHECK(Comparison::matches_sequence(dictionary_actual, &dictionary_type, dictionary_expected));

    const std::array remove_readd_actual{
        std::optional{dictionary_tick({{bg::details::TickOperation::add, 7, 40}})},
        std::optional{dictionary_tick({{bg::details::TickOperation::remove, 7}})},
        std::optional{dictionary_tick({{bg::details::TickOperation::add, 7, 40}})}};
    const bg::i64 key_seven{7};
    const bg::i64 value_forty{40};
    const std::array added_entry{literal::TimeSeriesLiteral(key_seven), literal::TimeSeriesLiteral(value_forty)};
    const std::array removed_entry{literal::TimeSeriesLiteral(key_seven),
                                   literal::TimeSeriesLiteral(literal::REMOVE)};
    const std::array added_entries{literal::TimeSeriesLiteral(std::span{added_entry})};
    const std::array removed_entries{literal::TimeSeriesLiteral(std::span{removed_entry})};
    const std::array remove_readd_expected{literal::TimeSeriesLiteral(std::span{added_entries}),
                                           literal::TimeSeriesLiteral(std::span{removed_entries}),
                                           literal::TimeSeriesLiteral(std::span{added_entries})};
    CHECK(Comparison::matches_sequence(remove_readd_actual, &dictionary_type, remove_readd_expected));

    const std::array removal_failure_actual{
        std::optional{dictionary_tick({{bg::details::TickOperation::add, 8, -1}})},
        std::optional{dictionary_tick({{bg::details::TickOperation::remove, 8}})}};
    const bg::i64 key_eight{8};
    const bg::i64 value_negative_one{-1};
    const std::array removal_failure_entry{literal::TimeSeriesLiteral(key_eight),
                                           literal::TimeSeriesLiteral(value_negative_one)};
    const std::array removal_failure_removed_entry{literal::TimeSeriesLiteral(key_eight),
                                                   literal::TimeSeriesLiteral(literal::REMOVE)};
    const std::array removal_failure_entries{literal::TimeSeriesLiteral(std::span{removal_failure_entry})};
    const std::array removal_failure_removed_entries{
        literal::TimeSeriesLiteral(std::span{removal_failure_removed_entry})};
    const std::array removal_failure_expected{literal::TimeSeriesLiteral(std::span{removal_failure_entries}),
                                              literal::TimeSeriesLiteral(std::span{removal_failure_removed_entries})};
    CHECK_FALSE(Comparison::matches_sequence(removal_failure_actual, &dictionary_type, removal_failure_expected));

    const auto &list_type = bg::time_series_list_type(type, 1, bg::TimeSeriesGuarantee::ordinary);
    const std::array list_actual{std::optional{list_tick(0, 50)},
                                 std::optional{stateful_extension_tick(0, false)}};
    const auto invalidation_count = stateful_comparison_stats.invalidated.size();
    const bg::item_id_t list_index{};
    const bg::i64 list_value{50};
    const std::array list_entry{literal::TimeSeriesLiteral(list_index), literal::TimeSeriesLiteral(list_value)};
    const std::array list_entries{literal::TimeSeriesLiteral(std::span{list_entry})};
    const std::array list_expected{literal::TimeSeriesLiteral(std::span{list_entries}),
                                   literal::TimeSeriesLiteral(literal::INVALID)};
    CHECK(Comparison::matches_sequence(list_actual, &list_type, list_expected));
    CHECK(stateful_comparison_stats.invalidated.size() == invalidation_count + 1);
    CHECK(stateful_comparison_stats.invalidated.back() == 1);
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
