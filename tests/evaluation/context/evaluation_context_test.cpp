#include <bg/global_state.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
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

TEST_CASE("an evaluation context exposes the outer global state",
          "[evaluation][context][global_state][feature:evaluation-context]") {
    const auto global_state_source = bg::node<global_state_source_definition>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { (void)global_state_source(); }, state);

    state.read([](const auto &values) { CHECK(*bg::scalar_cast<std::int64_t>(values.at("result")) == 42); });
}

struct scratch_pad_source_definition {
    void start(bg::EvaluationContext *context) const {
        alignas(std::max_align_t) auto local = std::array<std::byte, 64>{};
        auto local_storage = context->scratch().use_local_storage(local);

        auto *first = context->scratch().allocate(16, alignof(std::max_align_t));
        CHECK(first == local.data());

        context->scratch().release_last(first, 16);
        CHECK(context->scratch().allocate(16, alignof(std::max_align_t)) == first);

        auto *fallback = context->scratch().allocate(128, alignof(std::max_align_t));
        CHECK(fallback != local.data());
    }
};

TEST_CASE("an evaluation context scratch pad uses caller-owned storage first",
          "[evaluation][context][scratch][feature:evaluation-context]") {
    const auto scratch_pad_source = bg::node<scratch_pad_source_definition>();

    bg::run_graph([&] { scratch_pad_source(); });
}

} // namespace
