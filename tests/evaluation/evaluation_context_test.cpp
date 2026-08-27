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
        context->global_state().set(bg::util::symbol{"result"}, value);
        output.set(1);
    }

    void evaluate() const {}
};

inline const auto global_state_source = bg::node<global_state_source_definition>();

} // namespace

TEST_CASE("an evaluation context exposes the outer global state", "[evaluation][context][global_state]") {
    auto state = bg::GlobalState{};

    bg::run_graph([] { (void)global_state_source(); }, state);

    CHECK(bg::scalar_cast<std::int64_t>(state.at("result"))->value() == 42);
}
