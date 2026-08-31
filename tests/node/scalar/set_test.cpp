#include <bg/bg.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace {

struct inspect_scalar_set {
    bg::Scalar<bg::Set<std::int64_t>> values;

    void start(bg::EvaluationContext *context) const {
        auto result = std::int64_t{};
        values.for_each([&](const auto value) { result += value.value(); });

        context->global_state().write(
            [&](auto &state) { state.set(bg::util::symbol{"result"}, bg::ScalarValue{result}); });
    }

    void evaluate() const {}
};

TEST_CASE("a scalar set is available from packed graph scalar storage", "[node][scalar][set]") {
    const auto inspect = bg::node<inspect_scalar_set>();
    const auto input = bg::ScalarValue<bg::Set<std::int64_t>>{std::array<std::int64_t, 2>{1, 2}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(input); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 3); });
}

struct inspect_scalar_set_state {
    bg::State<bg::Set<std::int64_t>> values;

    void start(bg::EvaluationContext *context) const {
        (void)values->insert(1);
        (void)values->insert(2);

        context->global_state().write([&](auto &state) {
            state.set(bg::util::symbol{"result"}, bg::ScalarValue{static_cast<std::int64_t>(values->size())});
        });
    }

    void evaluate() const {}
};

TEST_CASE("scalar set state is available during node start", "[node][state][scalar][set]") {
    const auto inspect = bg::node<inspect_scalar_set_state>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { inspect(); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 2); });
}

struct scalar_set_source {
    bg::Scalar<bg::Set<std::int64_t>> value;
    bg::Out<bg::TS<bg::Set<std::int64_t>>> output;

    void start() const { output.set_value(value); }
    void evaluate() const {}
};

struct scalar_set_sink {
    bg::In<bg::TS<bg::Set<std::int64_t>>> input;

    void evaluate(bg::EvaluationContext *context) const {
        auto result = std::int64_t{};
        input.value_view().for_each([&](const auto value) { result += value.value(); });

        context->global_state().write(
            [&](auto &state) { state.set(bg::util::symbol{"result"}, bg::ScalarValue{result}); });
    }
};

TEST_CASE("a scalar set propagates through a value time series", "[node][time_series][scalar][set]") {
    const auto source = bg::node<scalar_set_source>();
    const auto sink = bg::node<scalar_set_sink>();
    const auto input = bg::ScalarValue<bg::Set<std::int64_t>>{std::array<std::int64_t, 2>{1, 2}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { sink(source(input)); }, state);

    state.read([](const auto &values) { CHECK(bg::scalar_cast<std::int64_t>(values.at("result")).value() == 3); });
}

} // namespace
