#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <cstdint>
#include <vector>

namespace {

using IntDictionary = bg::TSD<std::int64_t, bg::TS<int>>;

struct dict_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntDictionary> output;

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
    bg::In<IntDictionary> input;

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

struct key_sink_definition {
    bg::In<bg::TSS<std::int64_t>> input;

    void evaluate() const {
        sizes.push_back(input.size());

        auto added_count = std::size_t{};
        input.for_each_added([&](const auto &) { ++added_count; });
        added.push_back(added_count);

        auto removed_count = std::size_t{};
        input.for_each_removed([&](const auto &) { ++removed_count; });
        removed.push_back(removed_count);
    }

    inline static std::vector<std::size_t> sizes;
    inline static std::vector<std::size_t> added;
    inline static std::vector<std::size_t> removed;
};

} // namespace

TEST_CASE("a time series dictionary publishes item and key changes",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:update][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<dict_source_definition>();
    const auto dict_sink = bg::node<dict_sink_definition>();
    const auto key_sink = bg::node<key_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();
    key_sink_definition::sizes.clear();
    key_sink_definition::added.clear();
    key_sink_definition::removed.clear();

    bg::run_graph([&] {
        const auto dictionary = source();
        dict_sink(dictionary);
        key_sink(bg::keys(dictionary));
    });

    CHECK(dict_sink_definition::sizes == std::vector<std::size_t>{2, 2, 1});
    CHECK(dict_sink_definition::values == std::vector{10, 11, 11});
    CHECK(dict_sink_definition::added == std::vector<std::size_t>{2, 0, 0});
    CHECK(dict_sink_definition::removed == std::vector<std::size_t>{0, 0, 1});
    CHECK(dict_sink_definition::modified == std::vector<std::size_t>{0, 1, 0});

    CHECK(key_sink_definition::sizes == std::vector<std::size_t>{2, 1});
    CHECK(key_sink_definition::added == std::vector<std::size_t>{2, 0});
    CHECK(key_sink_definition::removed == std::vector<std::size_t>{0, 1});
}

namespace {

struct item_modified_sink_definition {
    bg::In<IntDictionary> input;

    void evaluate() const { modified.push_back(input.item_modified(1)); }

    inline static std::vector<bool> modified;
};

} // namespace

TEST_CASE("dictionary item queries distinguish addition from modification",
          "[evaluation][tsd][feature:collection-change][series:tsd][scalar:integer]"
          "[transition:addition][transition:update][binding:direct]") {
    const auto source = bg::node<dict_source_definition>();
    const auto sink = bg::node<item_modified_sink_definition>();
    item_modified_sink_definition::modified.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(item_modified_sink_definition::modified == std::vector{false, true, false});
}

namespace {

using IntReferenceDictionary = bg::TSD<std::int64_t, bg::REF<bg::TS<int>>>;

struct reference_dictionary_sink_definition {
    bg::In<IntReferenceDictionary> input;

    void evaluate() const {
        sizes.push_back(input.size());
        const auto *target = input[1].target();
        values.push_back(*static_cast<const int *>(target->target_value));
    }

    inline static std::vector<std::size_t> sizes;
    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("a value dictionary provides a source-backed reference representation",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:value-to-reference][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto sink = bg::node<reference_dictionary_sink_definition>();
    reference_dictionary_sink_definition::sizes.clear();
    reference_dictionary_sink_definition::values.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(reference_dictionary_sink_definition::sizes == std::vector<std::size_t>{2, 1});
    CHECK(reference_dictionary_sink_definition::values == std::vector{10, 11});
}

namespace {

struct reference_dictionary_rekey_definition {
    bg::In<IntReferenceDictionary> input;
    bg::Out<IntReferenceDictionary> output;

    void evaluate() const {
        input.for_each_added([&](const auto &key) { output[key.value() * 10].set(input[key]); });
        input.for_each_removed([&](const auto &key) { output.erase(key.value() * 10); });
    }
};

struct dereferenced_dictionary_sink_definition {
    bg::In<IntDictionary> input;

    void evaluate() const {
        sizes.push_back(input.size());
        values.push_back(input.contains(10) ? *input[10] : -1);
    }

    inline static std::vector<std::size_t> sizes;
    inline static std::vector<int> values;
};

struct explicit_reference_dictionary_pass_definition {
    bg::In<bg::REF<IntReferenceDictionary>> input;
    bg::Out<bg::REF<IntReferenceDictionary>> output;

    void evaluate() const { output.set(input); }
};

} // namespace

TEST_CASE("an independently built reference dictionary dereferences without intermediate evaluations",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:reference-to-value][binding:value-to-reference][transition:update][transition:removal]") {
    const auto source = bg::node<dict_source_definition>();
    const auto rekey = bg::node<reference_dictionary_rekey_definition>();
    const auto sink = bg::node<dereferenced_dictionary_sink_definition>();
    dereferenced_dictionary_sink_definition::sizes.clear();
    dereferenced_dictionary_sink_definition::values.clear();

    bg::run_graph([&] { sink(rekey(source())); });

    CHECK(dereferenced_dictionary_sink_definition::sizes == std::vector<std::size_t>{2, 2, 1});
    CHECK(dereferenced_dictionary_sink_definition::values == std::vector{10, 11, 11});
}

TEST_CASE("an explicit dictionary reference input receives a recursively adapted target",
          "[evaluation][tsd][reference][feature:reference][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-reference][binding:reference-to-value]"
          "[transition:update][transition:removal]") {
    const auto source = bg::node<dict_source_definition>();
    const auto pass = bg::node<explicit_reference_dictionary_pass_definition>();
    const auto sink = bg::node<dict_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();

    bg::run_graph([&] { sink(pass(source())); });

    CHECK(dict_sink_definition::sizes == std::vector<std::size_t>{2, 2, 1});
    CHECK(dict_sink_definition::values == std::vector{10, 11, 11});
}

namespace {

struct selected_value_sink_definition {
    bg::In<bg::TS<int>, bg::optional> input;

    void evaluate() const { values.push_back(input.valid() ? *input : -1); }

    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("dictionary item selection ignores changes to other keys",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto sink = bg::node<selected_value_sink_definition>();
    selected_value_sink_definition::values.clear();

    bg::run_graph([&] { sink(source()[1]); });

    CHECK(selected_value_sink_definition::values == std::vector{10, 11});
}

namespace {

struct delayed_selected_item_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntDictionary> output;

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

TEST_CASE("dictionary item selection follows a missing key through addition, removal, and restoration",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:addition]"
          "[transition:removal][transition:resurrection]") {
    const auto source = bg::node<delayed_selected_item_definition>();
    const auto sink = bg::node<selected_value_sink_definition>();
    selected_value_sink_definition::values.clear();

    bg::run_graph([&] { sink(source()[3]); });

    CHECK(selected_value_sink_definition::values == std::vector{30, -1, 31});
}

TEST_CASE("dictionary item selection flattens a contained reference",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto source = bg::node<dict_source_definition>();
    const auto rekey = bg::node<reference_dictionary_rekey_definition>();
    const auto sink = bg::node<selected_value_sink_definition>();
    selected_value_sink_definition::values.clear();

    bg::run_graph([&] { sink(rekey(source())[10]); });

    CHECK(selected_value_sink_definition::values == std::vector{10, 11});
}

namespace {

using ListDictionary = bg::TSD<std::int64_t, bg::TSL<bg::TS<int>, 2>>;

struct list_dictionary_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<ListDictionary> output;

    void start(bg::EvaluationContext *context) const {
        output[1][0].set(10);
        output[1][1].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const { output[1][0].set(11); }
};

struct selected_list_sink_definition {
    bg::In<bg::TSL<bg::TS<int>, 2>> input;

    void evaluate() const { values.push_back(*input[0] + *input[1]); }

    inline static std::vector<int> values;
};

} // namespace

TEST_CASE("dictionary item selection supports collection items",
          "[evaluation][tsd][tsl][reference][feature:getitem][series:tsd][series:fixed-tsl]"
          "[scalar:integer][binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto source = bg::node<list_dictionary_source_definition>();
    const auto sink = bg::node<selected_list_sink_definition>();
    selected_list_sink_definition::values.clear();

    bg::run_graph([&] { sink(source()[1]); });

    CHECK(selected_list_sink_definition::values == std::vector{30, 31});
}

namespace {

struct two_item_dictionary_definition {
    bg::In<bg::TS<int>> input;
    bg::Out<IntDictionary> output;

    void evaluate() const {
        output[1].set(*input);
        output[2].set(*input * 10);
    }
};

} // namespace

TEST_CASE("a time series key retargets dictionary item selection",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][series:value][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-value][transition:update]") {
    const auto dictionary = bg::node<two_item_dictionary_definition>();

    const auto graph = [&](bg::wp<bg::TS<int>> input, bg::wp<bg::TS<std::int64_t>> key) {
        return dictionary(input)[key];
    };

    const auto actual = bg::testing::test_graph(graph, std::array{10, 11, 12},
                                                std::array<std::int64_t, 3>{1, 2, 1});
    CHECK(actual.equals({10, 110, 12}));
}

namespace {

struct resurrecting_selected_item_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<IntDictionary> output;

    void start(bg::EvaluationContext *context) const {
        output[3].set(30);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(3);
        output[3].set(31);
    }
};

struct selected_reference_sink_definition {
    bg::In<bg::REF<bg::TS<int>>, bg::optional> input;

    void evaluate() const { validity.push_back(input.valid()); }

    inline static std::vector<bool> validity;
};

} // namespace

TEST_CASE("same-cycle item resurrection retains the selected reference",
          "[evaluation][tsd][reference][feature:getitem][series:tsd][scalar:integer]"
          "[binding:value-to-reference][binding:reference-to-reference][transition:resurrection]") {
    const auto source = bg::node<resurrecting_selected_item_definition>();
    const auto sink = bg::node<selected_reference_sink_definition>();
    selected_reference_sink_definition::validity.clear();

    bg::run_graph([&] { sink(source()[3]); });

    CHECK(selected_reference_sink_definition::validity == std::vector{true});
}

namespace {

using NestedIntDictionary = bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>;

struct nested_dict_source_definition {
    bg::Out<NestedIntDictionary> output;

    void start() const { output[1][2].set(12); }
    void evaluate() const {}
};

struct nested_dict_sink_definition {
    bg::In<NestedIntDictionary> input;

    void evaluate() const { value = *input[1][2]; }

    inline static int value{};
};

} // namespace

TEST_CASE("time series dictionaries can contain time series dictionaries",
          "[evaluation][tsd][series:tsd][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<nested_dict_source_definition>();
    const auto sink = bg::node<nested_dict_sink_definition>();
    nested_dict_sink_definition::value = 0;

    bg::run_graph([&] { sink(source()); });

    CHECK(nested_dict_sink_definition::value == 12);
}

namespace {

template <template <typename> typename Field> struct DictionaryBundle {
    Field<bg::TS<int>> value;
};

using BundleDictionary = bg::TSD<std::int64_t, bg::TSB<DictionaryBundle>>;

struct bundle_dict_source_definition {
    bg::Out<BundleDictionary> output;

    void start() const { output[1]->value.set(12); }
};

struct bundle_dict_sink_definition {
    bg::In<BundleDictionary> input;

    void evaluate() const { value = *input[1]->value; }

    inline static int value{};
};

} // namespace

TEST_CASE("time series dictionaries can contain runtime-sized time series",
          "[evaluation][tsd][series:tsd][tsb][series:tsb][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][schema:named]") {
    const auto source = bg::node<bundle_dict_source_definition>();
    const auto sink = bg::node<bundle_dict_sink_definition>();
    bundle_dict_sink_definition::value = 0;

    bg::run_graph([&] { sink(source()); });

    CHECK(bundle_dict_sink_definition::value == 12);
}

namespace {

struct canceled_add_source_definition {
    bg::Out<IntDictionary> output;

    void start() const {
        output[1].set(10);
        output.erase(1);
    }

    void evaluate() const {}
};

struct evaluation_count_sink_definition {
    bg::In<IntDictionary> input;

    void evaluate() const { ++count; }

    inline static int count{};
};

} // namespace

TEST_CASE("adding and removing a new dictionary item in one invocation produces a tick",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:removal]"
          "[transition:update][scalar:integer][representation:native][declaration:concrete]"
          "[binding:direct][transition:addition]") {
    const auto source = bg::node<canceled_add_source_definition>();
    const auto sink = bg::node<evaluation_count_sink_definition>();
    evaluation_count_sink_definition::count = 0;

    bg::run_graph([&] { sink(source()); });

    CHECK(evaluation_count_sink_definition::count == 1);
}

namespace {

struct resurrecting_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<IntDictionary> output;

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
    const auto key_sink = bg::node<key_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();
    key_sink_definition::sizes.clear();
    key_sink_definition::added.clear();
    key_sink_definition::removed.clear();

    bg::run_graph([&] {
        const auto dictionary = source();
        dict_sink(dictionary);
        key_sink(bg::keys(dictionary));
    });

    CHECK(dict_sink_definition::values == std::vector{10, 20});
    CHECK(dict_sink_definition::added == std::vector<std::size_t>{1, 0});
    CHECK(dict_sink_definition::removed == std::vector<std::size_t>{0, 0});
    CHECK(dict_sink_definition::modified == std::vector<std::size_t>{0, 1});
    CHECK(key_sink_definition::sizes == std::vector<std::size_t>{1, 1});
    CHECK(key_sink_definition::added == std::vector<std::size_t>{1, 0});
    CHECK(key_sink_definition::removed == std::vector<std::size_t>{0, 0});
}

namespace {

struct nested_item_resurrecting_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::Out<NestedIntDictionary> output;

    void start(bg::EvaluationContext *context) const {
        output[1][2].set(12);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate() const {
        output.erase(1);
        output[1];
    }
};

struct nested_item_resurrection_sink_definition {
    bg::In<NestedIntDictionary> input;

    void evaluate() const {
        auto modified_count = std::size_t{};
        input.for_each_modified([&](const auto &) { ++modified_count; });
        modified.push_back(modified_count);
    }

    inline static std::vector<std::size_t> modified;
};

} // namespace

TEST_CASE("resurrecting a dictionary collection item reports a modification",
          "[evaluation][tsd][series:tsd][feature:collection-change][transition:resurrection]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<nested_item_resurrecting_source_definition>();
    const auto sink = bg::node<nested_item_resurrection_sink_definition>();
    nested_item_resurrection_sink_definition::modified.clear();

    bg::run_graph([&] { sink(source()); });

    CHECK(nested_item_resurrection_sink_definition::modified == std::vector<std::size_t>{0, 1});
}

namespace {

using GenericValueDictionary = bg::TSD<std::int64_t, bg::TS<bg::SCALAR>>;

struct generic_dict_copy_definition {
    bg::In<GenericValueDictionary> input;
    bg::Out<GenericValueDictionary> output;

    void evaluate() const {
        input.for_each([&](const auto &key, const auto &item) { output[key].set_value(item.value_view()); });
    }
};

struct one_item_dict_source_definition {
    bg::Out<IntDictionary> output;

    void start() const { output[1].set(10); }
    void evaluate() const {}
};

} // namespace

TEST_CASE("a generic dictionary view uses its resolved item type",
          "[evaluation][tsd][series:tsd][generic][declaration:generic][feature:collection-change]"
          "[scalar:integer][representation:native][binding:direct]") {
    const auto source = bg::node<one_item_dict_source_definition>();
    const auto copy = bg::node<generic_dict_copy_definition>();
    const auto sink = bg::node<dict_sink_definition>();
    dict_sink_definition::sizes.clear();
    dict_sink_definition::values.clear();
    dict_sink_definition::added.clear();
    dict_sink_definition::removed.clear();
    dict_sink_definition::modified.clear();

    bg::run_graph([&] { sink(copy(source())); });

    CHECK(dict_sink_definition::sizes == std::vector<std::size_t>{1});
    CHECK(dict_sink_definition::values == std::vector{10});
}

namespace {

struct empty_dict_source_definition {
    static constexpr bool uses_future_scheduling = true;

    bg::State<int> step;
    bg::Out<IntDictionary> output;

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
    bg::In<IntDictionary, bg::optional> input;

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

struct optional_key_sink_definition {
    bg::In<bg::TSS<std::int64_t>, bg::optional> input;

    void evaluate() const { validity.push_back(input.valid()); }

    inline static std::vector<bool> validity;
};

} // namespace

TEST_CASE("a dictionary and its key set become valid on their first empty tick and remain valid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][scalar:integer]"
          "[representation:native][declaration:concrete][binding:direct][transition:first-tick]") {
    const auto source = bg::node<empty_dict_source_definition>();
    const auto dict_sink = bg::node<optional_dict_sink_definition>();
    const auto key_sink = bg::node<optional_key_sink_definition>();
    optional_dict_sink_definition::validity.clear();
    optional_dict_sink_definition::sizes.clear();
    optional_dict_sink_definition::added.clear();
    optional_dict_sink_definition::removed.clear();
    optional_key_sink_definition::validity.clear();

    bg::run_graph([&] {
        const auto dictionary = source();
        dict_sink(dictionary);
        key_sink(bg::keys(dictionary));
    });

    CHECK(optional_dict_sink_definition::validity == std::vector{true, true, true});
    CHECK(optional_dict_sink_definition::sizes == std::vector<std::size_t>{0, 2, 0});
    CHECK(optional_dict_sink_definition::added == std::vector<std::size_t>{0, 2, 0});
    CHECK(optional_dict_sink_definition::removed == std::vector<std::size_t>{0, 0, 2});
    CHECK(optional_key_sink_definition::validity == std::vector{true, true, true});
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
    bg::Out<IntDictionary> output;

    void evaluate() const { output[1].set(*input); }
};

} // namespace

TEST_CASE("a dictionary preserves its value when a required input becomes invalid",
          "[evaluation][tsd][series:tsd][validity][feature:collection-change][validity:required]"
          "[scalar:integer][representation:native][declaration:concrete][binding:direct]") {
    const auto source = bg::node<invalidating_dict_input_definition>();
    const auto dict_from_value = bg::node<dict_from_value_definition>();
    const auto sink = bg::node<optional_dict_sink_definition>();
    optional_dict_sink_definition::validity.clear();
    optional_dict_sink_definition::sizes.clear();
    optional_dict_sink_definition::added.clear();
    optional_dict_sink_definition::removed.clear();

    bg::run_graph([&] { sink(dict_from_value(source())); });

    CHECK(optional_dict_sink_definition::validity == std::vector{true});
    CHECK(optional_dict_sink_definition::sizes == std::vector<std::size_t>{1});
}
