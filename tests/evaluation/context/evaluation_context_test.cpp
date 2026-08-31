#include <bg/global_state.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

struct global_state_source_definition {
    bg::Out<bg::TS<int>> output;

    void start(bg::EvaluationContext *context) const {
        const auto value = bg::ScalarValue{42};
        context->global_state().write([&](auto &state) { state.set(bg::util::symbol{"result"}, value); });
        output.set(1);
    }

    void evaluate() const {}
};

TEST_CASE("an evaluation context exposes the outer global state", "[evaluation][context][global_state]") {
    const auto global_state_source = bg::node<global_state_source_definition>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { (void)global_state_source(); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<std::int64_t>(values.at("result"))->value() == 42);
    });
}

} // namespace
