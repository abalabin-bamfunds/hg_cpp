#include <bg/bg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace {

using IntDictionary = bg::Dict<std::int64_t, std::int64_t>;
using IntDictionaryEntry = std::pair<std::int64_t, std::int64_t>;

struct inspect_scalar_dict {
    bg::Scalar<IntDictionary> values;

    void start(bg::EvaluationContext *context) const {
        auto result = std::int64_t{};
        values.for_each([&](const auto, const auto value) { result += value.value(); });

        context->global_state().write(
            [&](auto &state) { state.set(bg::util::symbol{"result"}, bg::ScalarValue{result}); });
    }

    void evaluate() const {}
};

TEST_CASE("a scalar dictionary is available from packed graph scalar storage",
          "[node][scalar][dict][scalar:dict][feature:state]") {
    const auto inspect = bg::node<inspect_scalar_dict>();
    const auto input = bg::ScalarValue<IntDictionary>{std::array{IntDictionaryEntry{1, 2}, IntDictionaryEntry{3, 4}}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(input); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 6); });
}

struct inspect_scalar_dict_state {
    bg::State<IntDictionary> values;

    void start(bg::EvaluationContext *context) const {
        values->insert_or_assign(1, 2);
        values->insert_or_assign(3, 4);

        context->global_state().write([&](auto &state) {
            state.set(bg::util::symbol{"result"}, bg::ScalarValue{values->at(1) + values->at(3)});
        });
    }

    void evaluate() const {}
};

TEST_CASE("scalar dictionary state is available during node start",
          "[node][state][scalar][dict][scalar:dict][feature:state][transition:start]") {
    const auto inspect = bg::node<inspect_scalar_dict_state>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 6); });
}

struct scalar_dict_source {
    bg::Scalar<IntDictionary> value;
    bg::Out<bg::TS<IntDictionary>> output;

    void start() const { output.set_value(value); }
    void evaluate() const {}
};

struct scalar_dict_sink {
    bg::In<bg::TS<IntDictionary>> input;

    void evaluate(bg::EvaluationContext *context) const {
        auto result = std::int64_t{};
        input.value_view().for_each([&](const auto, const auto value) { result += value.value(); });

        context->global_state().write(
            [&](auto &state) { state.set(bg::util::symbol{"result"}, bg::ScalarValue{result}); });
    }
};

TEST_CASE("a scalar dictionary propagates through a value time series",
          "[node][time_series][scalar][dict][scalar:dict][feature:state]") {
    const auto source = bg::node<scalar_dict_source>();
    const auto sink = bg::node<scalar_dict_sink>();
    const auto input = bg::ScalarValue<IntDictionary>{std::array{IntDictionaryEntry{1, 2}, IntDictionaryEntry{3, 4}}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { sink(source(input)); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 6); });
}

} // namespace
