#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
        values.push_back(input.contains(1) ? input[1].value() : -1);

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

TEST_CASE("a time series dictionary publishes item and key changes", "[evaluation][tsd]") {
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

using NestedIntDictionary = bg::TSD<std::int64_t, bg::TSD<std::int64_t, bg::TS<int>>>;

struct nested_dict_source_definition {
    bg::Out<NestedIntDictionary> output;

    void start() const { output[1][2].set(12); }
    void evaluate() const {}
};

struct nested_dict_sink_definition {
    bg::In<NestedIntDictionary> input;

    void evaluate() const { value = input[1][2].value(); }

    inline static int value{};
};

} // namespace

TEST_CASE("time series dictionaries can contain time series dictionaries", "[evaluation][tsd]") {
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

    void evaluate() const { value = input[1]->value.value(); }

    inline static int value{};
};

} // namespace

TEST_CASE("time series dictionaries can contain runtime-sized time series", "[evaluation][tsd][tsb]") {
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

TEST_CASE("adding and removing a new dictionary item in one invocation produces a tick", "[evaluation][tsd]") {
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

TEST_CASE("resurrecting a dictionary item reports a modification", "[evaluation][tsd]") {
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

TEST_CASE("resurrecting a dictionary collection item reports a modification", "[evaluation][tsd]") {
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

TEST_CASE("a generic dictionary view uses its resolved item type", "[evaluation][tsd][generic]") {
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
          "[evaluation][tsd][validity]") {
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

    void evaluate() const { output[1].set(input.value()); }
};

} // namespace

TEST_CASE("a dictionary preserves its value when a required input becomes invalid", "[evaluation][tsd][validity]") {
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
