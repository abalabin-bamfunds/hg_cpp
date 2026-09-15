#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

struct dict_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output[1].set(10);
        output[2].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[1].set(11);
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output.erase(2);
        }
        ++*step;
    }
};

struct dict_sink_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;

    void evaluate() const {
        sizes.push_back(input.size());
        values.push_back(input.contains(1) ? *input[1] : -1);

        auto added_count = std::size_t{};
        input.for_each_added([&](const auto &) { ++added_count; });
        added.push_back(added_count);

        auto removed_count = std::size_t{};
        input.for_each_removed([&](const auto &) { ++removed_count; });
        removed.push_back(removed_count);

        auto modified_count = std::size_t{};
        input.for_each_modified([&](const auto &) { ++modified_count; });
        modified.push_back(modified_count);
    }

    inline static std::vector<std::size_t> sizes;
    inline static std::vector<int> values;
    inline static std::vector<std::size_t> added;
    inline static std::vector<std::size_t> removed;
    inline static std::vector<std::size_t> modified;
};

} // namespace

TEST_CASE("a time series dictionary publishes item and key changes",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:update][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<dict_source_definition>();
    const auto dict_sink = bg::node<dict_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();

    bg::run_graph([&] { dict_sink(source()); });

    CHECK(dict_sink_definition::sizes == std::vector<std::size_t>{2, 2, 1});
    CHECK(dict_sink_definition::values == std::vector{10, 11, 11});
    CHECK(dict_sink_definition::added == std::vector<std::size_t>{2, 0, 0});
    CHECK(dict_sink_definition::removed == std::vector<std::size_t>{0, 0, 1});
    CHECK(dict_sink_definition::modified == std::vector<std::size_t>{0, 1, 0});
}

namespace {

struct item_modified_value_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(input.item_modified(1)); }
};

} // namespace

TEST_CASE("dictionary item queries distinguish addition from modification",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer]"
          "[transition:addition][transition:update][binding:direct]") {
    const auto source = bg::node<dict_source_definition>();
    const auto modified = bg::node<item_modified_value_definition>();
    const auto graph = [&] { return modified(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({false, true, false}));
}

namespace {

struct reference_dictionary_summary_definition {
    bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const {
        const auto resolved = bg::details::resolve_reference(input[1].value());
        output.set(static_cast<int>(input.size() * 100) + *static_cast<const int *>(resolved.storage));
    }
};

} // namespace

TEST_CASE("a value dictionary provides a source-backed reference representation",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:value-to-reference][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto summarize = bg::node<reference_dictionary_summary_definition>();
    const auto graph = [&] { return summarize(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({210, std::nullopt, 111}));
}

namespace {

struct reference_dictionary_rekey_definition {
    bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input;
    bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output;

    void evaluate() const {
        input.for_each_added([&](const auto &key) { output[key.value() * 10].copy(input[key]); });
        input.for_each_removed([&](const auto &key) { output.erase(key.value() * 10); });
    }
};

struct dereferenced_dictionary_summary_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(static_cast<int>(input.size() * 100) + (input.contains(10) ? *input[10] : -1)); }
};

struct explicit_reference_dictionary_pass_definition {
    bg::In<bg::REF<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>>> input;
    bg::Out<bg::REF<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>>> output;

    void evaluate() const { output.copy(input); }
};

struct dictionary_summary_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(static_cast<int>(input.size() * 100) + *input[1]); }
};

} // namespace

TEST_CASE("rekeying a reference dictionary preserves item updates and removals",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:reference-to-value][binding:value-to-reference][transition:update][transition:removal]") {
    const auto source = bg::node<dict_source_definition>();
    const auto rekey = bg::node<reference_dictionary_rekey_definition>();
    const auto summarize = bg::node<dereferenced_dictionary_summary_definition>();
    const auto graph = [&] { return summarize(rekey(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({210, 211, 111}));
}

TEST_CASE("source dictionary item routes batch fan-out to reference dictionaries",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:reference-to-value][binding:value-to-reference][shape:fan-out][transition:update]") {
    const auto source = bg::node(
        [](bg::In<bg::TS<int>> input, bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output) {
            for (auto key = std::int64_t{}; key != 31; ++key) {
                output[key].set(*input + static_cast<int>(key));
            }
            if (*input == 2) {
                output[31].set(33);
            } else if (*input == 3) {
                output.erase(31);
            }
        });
    const auto rekey = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input, bg::Scalar<std::int64_t> offset,
           bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output) {
            input.for_each_added([&](const auto &key) { output[key.value() + *offset].copy(input[key]); });
            input.for_each_removed([&](const auto &key) { output.erase(key.value() + *offset); });
        });
    const auto sum = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) {
            auto result = 0;
            input.for_each_value([&](const auto &value) { result += *value; });
            output.set(result);
        });
    const auto add = bg::node(
        [](bg::In<bg::TS<int>> left, bg::In<bg::TS<int>> right, bg::Out<bg::TS<int>> output) {
            output.set(*left + *right);
        });
    const auto graph = [&](bg::wp<bg::TS<int>> input) {
        const auto values = source(input);
        return add(sum(rekey(values, std::int64_t{100})), sum(rekey(values, std::int64_t{200})));
    };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({992, 1120, 1116}));
}

namespace {

struct increment_reference_target_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input + 1); }
};

struct scale_reference_target_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input * 10); }
};

struct standalone_reference_dictionary_definition {
    bg::In<bg::REF<bg::TS<int>>> first;
    bg::In<bg::REF<bg::TS<int>>> second;
    bg::Out<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> output;

    void evaluate() const {
        output[1].copy(first);
        output[2].copy(second);
    }
};

struct sum_dictionary_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[1] + *input[2]); }
};

} // namespace

TEST_CASE("a reference dictionary dereferences unrelated node outputs",
          "[evaluation][tsd][reference][feature:reference][series:tsd][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto increment = bg::node<increment_reference_target_definition>();
    const auto scale = bg::node<scale_reference_target_definition>();
    const auto references = bg::node<standalone_reference_dictionary_definition>();
    const auto sum = bg::node<sum_dictionary_definition>();
    const auto graph = [&](bg::wp<bg::TS<int>> input) { return sum(references(increment(input), scale(input))); };

    const auto actual = bg::testing::test_graph(graph, std::array{1, 2, 3});

    CHECK(actual.equals({12, 23, 34}));
}

TEST_CASE("an explicit dictionary reference input receives a recursively adapted target",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update][transition:removal]") {
    const auto source = bg::node<dict_source_definition>();
    const auto pass = bg::node<explicit_reference_dictionary_pass_definition>();
    const auto summarize = bg::node<dictionary_summary_definition>();
    const auto graph = [&] { return summarize(pass(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({210, 211, 111}));
}

TEST_CASE("a node can read one dictionary item",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) {
        auto changed = input.item_modified(1);
        input.for_each_added([&](const auto &key) { changed = changed || key.value() == 1; });
        if (input.contains(1) && changed) {
            output.set(*input[1]);
        }
    });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 11}));
}

namespace {

struct delayed_selected_item_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[3].set(30);
        } else if (*step == 1) {
            output.erase(3);
        } else {
            output[3].set(31);
        }

        if (*step != 2) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
        ++*step;
    }
};

} // namespace

TEST_CASE("a node observes dictionary item addition, removal, and restoration",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct][transition:addition]"
          "[transition:removal][transition:resurrection]") {
    const auto source = bg::node<delayed_selected_item_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) {
        if (input.contains(3)) {
            output.set(*input[3]);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({std::nullopt, 30, std::nullopt, 31}));
}

TEST_CASE("a node can publish a contained dictionary reference",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto rekey = bg::node<reference_dictionary_rekey_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>> input,
                                    bg::Out<bg::REF<bg::TS<int>>> output) { output.copy(input[10]); });
    const auto graph = [&] { return select(rekey(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({10, 11}));
}

namespace {

struct list_dictionary_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSD<std::int64_t, bg::TSL<bg::TS<int>, 2>>> output;

    void start(bg::EvaluationContext *context) const {
        output[1][0].set(10);
        output[1][1].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1][0].set(11); }
};

struct selected_list_sum_definition {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[0] + *input[1]); }
};

} // namespace

TEST_CASE("a node can read a collection-valued dictionary item",
          "[evaluation][tsd][tsl][feature:getitem][series:tsd][series:fixed-tsl][scalar:integer]"
          "[binding:direct][transition:update]") {
    const auto source = bg::node<list_dictionary_source_definition>();
    const auto sum = bg::node<selected_list_sum_definition>();
    const auto select = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TSL<bg::TS<int>, 2>>> input, bg::Out<bg::TSL<bg::TS<int>, 2>> output) {
            const auto item = input[1];
            output[0].set(*item[0]);
            output[1].set(*item[1]);
        });
    const auto graph = [&] { return sum(select(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({30, 31}));
}

namespace {

struct two_item_dictionary_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void evaluate() const {
        output[1].set(*input);
        output[2].set(*input * 10);
    }
};

} // namespace

TEST_CASE("a node can select a dictionary item using a time series key",
          "[evaluation][tsd][feature:getitem][series:tsd][series:value][scalar:integer]"
          "[binding:direct][transition:update]") {
    const auto dictionary = bg::node<two_item_dictionary_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::In<bg::TS<std::int64_t>> key,
                                    bg::Out<bg::TS<int>> output) { output.set(*input[*key]); });

    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<std::int64_t>> key) {
        return select(dictionary(input), key);
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 11, 12}, std::array<std::int64_t, 3>{1, 2, 1});
    CHECK(actual.equals({10, 110, 12}));
}

namespace {

struct resurrecting_selected_item_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output[3].set(30);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(3);
        output[3].set(31);
    }
};

} // namespace

TEST_CASE("a node observes same-cycle dictionary item resurrection",
          "[evaluation][tsd][feature:getitem][series:tsd][scalar:integer][binding:direct]"
          "[transition:resurrection]") {
    const auto source = bg::node<resurrecting_selected_item_definition>();
    const auto select = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) { output.set(*input[3]); });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({30, 31}));
}

namespace {

struct nested_dict_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output;

    void start() const { output[1][2].set(12); }
    void evaluate() const {}
};

} // namespace

TEST_CASE("time series dictionaries can contain time series dictionaries",
          "[evaluation][tsd][series:tsd][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<nested_dict_source_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> input,
                                    bg::Out<bg::TS<int>> output) { output.set(*input[1][2]); });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({12}));
}

namespace {

template <template <typename> typename Field> struct DictionaryBundle {
    Field<bg::TS<int>> value;
};

struct bundle_dict_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TSB<DictionaryBundle>>> output;

    void start() const { output[1]->value.set(12); }
};

struct bundle_dict_value_definition {
    bg::In<bg::TSD<std::int64_t, bg::TSB<DictionaryBundle>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(*input[1]->value); }
};

} // namespace

TEST_CASE("time series dictionaries can contain runtime-sized time series",
          "[evaluation][tsd][series:tsd][tsb][series:tsb][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][schema:named]") {
    const auto source = bg::node<bundle_dict_source_definition>();
    const auto extract = bg::node<bundle_dict_value_definition>();
    const auto graph = [&] { return extract(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({12}));
}

namespace {

struct canceled_add_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start() const {
        output[1].set(10);
        output.erase(1);
    }

    void evaluate() const {}
};

struct evaluation_marker_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;
    bg::Out<bg::TS<bool>> output;

    void evaluate() const { output.set(true); }
};

} // namespace

TEST_CASE("adding and removing a new dictionary item in one invocation produces a tick",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:removal]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct][transition:addition]") {
    const auto source = bg::node<canceled_add_source_definition>();
    const auto marker = bg::node<evaluation_marker_definition>();
    const auto graph = [&] { return marker(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({true}));
}

namespace {

struct resurrecting_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output[1].set(10);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(1);
        output[1].set(20);
    }
};

} // namespace

TEST_CASE("resurrecting a dictionary item reports a modification",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:resurrection]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<resurrecting_source_definition>();
    const auto dict_sink = bg::node<dict_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();

    bg::run_graph([&] { dict_sink(source()); });

    CHECK(dict_sink_definition::values == std::vector{10, 20});
    CHECK(dict_sink_definition::added == std::vector<std::size_t>{1, 0});
    CHECK(dict_sink_definition::removed == std::vector<std::size_t>{0, 0});
    CHECK(dict_sink_definition::modified == std::vector<std::size_t>{0, 1});
}

namespace {

struct nested_item_resurrecting_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> output;

    void start(bg::EvaluationContext *context) const {
        output[1][2].set(12);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(1);
        output[1];
    }
};

struct nested_item_resurrection_count_definition {
    bg::In<bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const {
        auto modified_count = int{};
        input.for_each_modified([&](const auto &) { ++modified_count; });
        output.set(modified_count);
    }
};

} // namespace

TEST_CASE("resurrecting a dictionary collection item reports a modification",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:resurrection]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<nested_item_resurrecting_source_definition>();
    const auto count = bg::node<nested_item_resurrection_count_definition>();
    const auto graph = [&] { return count(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({0, 1}));
}

namespace {

struct generic_dict_copy_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<bg::SCALAR>>> input;
    bg::Out<bg::TSD<std::int64_t, bg::TS<bg::SCALAR>>> output;

    void evaluate() const {
        input.for_each([&](const auto &key, const auto &item) { output[key].set_value(item.value_view()); });
    }
};

struct one_item_dict_source_definition {
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start() const { output[1].set(10); }
    void evaluate() const {}
};

} // namespace

TEST_CASE("a generic dictionary view uses its resolved item type",
          "[evaluation][tsd][series:tsd][generic][declaration:generic][feature:collection-change]"
          "[scalar:integer][representation:native][binding:direct]") {
    const auto source = bg::node<one_item_dict_source_definition>();
    const auto copy = bg::node<generic_dict_copy_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<std::int64_t, bg::TS<bg::SCALAR>>> input,
                                    bg::Out<bg::TS<bg::SCALAR>> output) { output.set_value(input[1].value_view()); });
    const auto graph = [&] { return select(copy(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals<int>({10}));
}

namespace {

struct empty_dict_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output.clear();
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[1].set(10);
            output[2].set(20);
        } else {
            output.clear();
        }

        if (*step != 2) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
        ++*step;
    }
};

struct optional_dict_sink_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>, bg::optional> input;

    void evaluate() const {
        validity.push_back(input.valid());
        sizes.push_back(input.size());

        auto added_count = std::size_t{};
        input.for_each_added([&](const auto &) { ++added_count; });
        added.push_back(added_count);

        auto removed_count = std::size_t{};
        input.for_each_removed([&](const auto &) { ++removed_count; });
        removed.push_back(removed_count);
    }

    inline static std::vector<bool> validity;
    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::size_t> added;
    inline static std::vector<std::size_t> removed;
};

} // namespace

TEST_CASE("a dictionary becomes valid on its first empty tick and remains valid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][transition:first-tick]") {
    const auto source = bg::node<empty_dict_source_definition>();
    const auto dict_sink = bg::node<optional_dict_sink_definition>();
    optional_dict_sink_definition::validity.clear();
    optional_dict_sink_definition::sizes.clear();
    optional_dict_sink_definition::added.clear();
    optional_dict_sink_definition::removed.clear();

    bg::run_graph([&] { dict_sink(source()); });

    CHECK(optional_dict_sink_definition::validity == std::vector{true, true, true});
    CHECK(optional_dict_sink_definition::sizes == std::vector<std::size_t>{0, 2, 0});
    CHECK(optional_dict_sink_definition::added == std::vector<std::size_t>{0, 2, 0});
    CHECK(optional_dict_sink_definition::removed == std::vector<std::size_t>{0, 0, 2});
}

namespace {

struct invalidating_dict_input_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        output.set(1);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.invalidate(); }
};

struct dict_from_value_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<bg::TSD<std::int64_t, bg::TS<int>>> output;

    void evaluate() const { output[1].set(*input); }
};

} // namespace

TEST_CASE("a dictionary preserves its value when a required input becomes invalid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][validity:required]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<invalidating_dict_input_definition>();
    const auto dict_from_value = bg::node<dict_from_value_definition>();
    const auto select = bg::node(
        [](bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) { output.set(*input[1]); });
    const auto graph = [&] { return select(dict_from_value(source())); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({1}));
}

namespace {

struct dictionary_enumeration_snapshot {
    std::vector<std::int64_t> keys;
    std::vector<bg::item_id_t> ids;
    std::vector<int> values;
    std::vector<std::pair<std::int64_t, int>> key_values;
    std::vector<std::pair<bg::item_id_t, int>> id_values;
};

struct dictionary_enumeration_sink_definition {
    bg::In<bg::TSD<std::int64_t, bg::TS<int>>> input;

    void evaluate() const {
        auto snapshot = dictionary_enumeration_snapshot{};
        input.for_each_key([&](const auto &key) { snapshot.keys.push_back(*key); });
        input.for_each_id([&](bg::item_id_t id) { snapshot.ids.push_back(id); });
        input.for_each_value([&](const auto &value) { snapshot.values.push_back(*value); });
        input.for_each_key_value(
            [&](const auto &key, const auto &value) { snapshot.key_values.emplace_back(*key, *value); });
        input.for_each_id_value(
            [&](bg::item_id_t id, const auto &value) { snapshot.id_values.emplace_back(id, *value); });
        snapshots.push_back(std::move(snapshot));
    }

    inline static std::vector<dictionary_enumeration_snapshot> snapshots;
};

} // namespace

TEST_CASE("dictionary enumeration exposes keys, ids, values, and their associations",
          "[evaluation][series:tsd][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][binding:direct]") {
    const auto source = bg::node<dict_source_definition>();
    const auto sink = bg::node<dictionary_enumeration_sink_definition>();
    dictionary_enumeration_sink_definition::snapshots.clear();

    bg::run_graph([&] { sink(source()); });

    REQUIRE(dictionary_enumeration_sink_definition::snapshots.size() == 3);
    auto first = dictionary_enumeration_sink_definition::snapshots.front();
    std::ranges::sort(first.keys);
    std::ranges::sort(first.values);
    std::ranges::sort(first.key_values);
    std::ranges::sort(first.ids);

    auto id_value_ids = std::vector<bg::item_id_t>{};
    auto id_values = std::vector<int>{};
    for (const auto &[id, value] : first.id_values) {
        id_value_ids.push_back(id);
        id_values.push_back(value);
    }
    std::ranges::sort(id_value_ids);
    std::ranges::sort(id_values);

    CHECK(first.keys == std::vector<std::int64_t>{1, 2});
    CHECK(first.values == std::vector{10, 20});
    CHECK(first.key_values == std::vector<std::pair<std::int64_t, int>>{{1, 10}, {2, 20}});
    REQUIRE(first.ids.size() == 2);
    CHECK(first.ids[0] != first.ids[1]);
    CHECK(id_value_ids == first.ids);
    CHECK(id_values == first.values);

    auto last = dictionary_enumeration_sink_definition::snapshots.back();
    std::ranges::sort(last.keys);
    std::ranges::sort(last.values);
    CHECK(last.keys == std::vector<std::int64_t>{1});
    CHECK(last.values == std::vector{11});
}

TEST_CASE("an invalid dictionary input has no entries to enumerate",
          "[value][series:tsd][feature:time-series-view][validity:optional]") {
    const auto input = bg::In<bg::TSD<std::int64_t, bg::TS<int>>>{};
    auto calls = std::array<std::size_t, 5>{};

    input.for_each_key([&](const auto &) { ++calls[0]; });
    input.for_each_id([&](bg::item_id_t) { ++calls[1]; });
    input.for_each_value([&](const auto &) { ++calls[2]; });
    input.for_each_key_value([&](const auto &, const auto &) { ++calls[3]; });
    input.for_each_id_value([&](bg::item_id_t, const auto &) { ++calls[4]; });

    CHECK(calls == std::array<std::size_t, 5>{});
}

namespace {

struct exact_int_dictionary_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<bg::TSD<int, bg::TS<int>>> output;

    void start(bg::EvaluationContext *context) const {
        output[7].set(70);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output.erase(7); }
};

struct exact_int_dictionary_read_definition {
    bg::In<bg::TSD<int, bg::TS<int>>> input;
    bg::Out<bg::TS<int>> output;

    void evaluate() const { output.set(input.contains(7) && !input.item_modified(7) ? *input[7] : -1); }
};

} // namespace

TEST_CASE("a dictionary view accepts its exact non-canonical key type",
          "[evaluation][series:tsd][feature:time-series-view][feature:collection-change]"
          "[scalar:integer][transition:addition][transition:removal][binding:direct]") {
    const auto source = bg::node<exact_int_dictionary_source_definition>();
    const auto read = bg::node<exact_int_dictionary_read_definition>();
    const auto graph = [&] { return read(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({70, -1}));
}

TEST_CASE("a dictionary input accepts its exact non-canonical key type",
          "[evaluation][feature:getitem][series:tsd][scalar:integer][binding:direct]") {
    const auto source = bg::node<exact_int_dictionary_source_definition>();
    const auto select = bg::node([](bg::In<bg::TSD<int, bg::TS<int>>> input, bg::Out<bg::TS<int>> output) {
        if (input.contains(7)) {
            output.set(*input[7]);
        } else {
            output.invalidate();
        }
    });
    const auto graph = [&] { return select(source()); };

    const auto actual = bg::testing::test_graph(graph);

    CHECK(actual.equals({70, std::nullopt}));
}
