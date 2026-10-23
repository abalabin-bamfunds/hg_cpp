#include <bg/global_state.hpp>
#include <bg/graph.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bg;
using namespace bg::literals;

namespace {

TEST_CASE("an evaluation context exposes the outer global state",
          "[evaluation][context][global_state][feature:evaluation-context]") {
    struct global_state_source_definition {
        Out<TS<i64>> output;

        void start(EvaluationContext *context) const {
            const auto value = ScalarValue{42};
            context->global_state().write([&](auto &state) { state.set(bg::util::symbol{"result"}, value); });
            output.set(1);
        }

        void evaluate() const {}
    };

    const auto global_state_source = bg::node<global_state_source_definition>();
    auto state = GlobalState{};

    bg::run_graph([&] { (void)global_state_source(); }, GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) { CHECK(*bg::scalar_cast<i64>(values.at("result")) == 42); });
}

struct invocation_scratch_callback_state {
    extension::InvocationEndRegistration registration;
    int *value{};
    int callback_count{};
};

invocation_scratch_callback_state invocation_scratch_state;

void inspect_invocation_scratch(void *receiver, EvaluationContext &) {
    auto &state = *static_cast<invocation_scratch_callback_state *>(receiver);
    CHECK(*state.value == 42);
    state.value = nullptr;
    ++state.callback_count;
}

struct invocation_scratch_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        auto *value = static_cast<int *>(context->scratch().allocate(sizeof(int), alignof(int)));
        *value = 42;
        output.set(1);
        invocation_scratch_state.value = value;
        context->add_invocation_end_callback(invocation_scratch_state.registration, &invocation_scratch_state,
                                             inspect_invocation_scratch);
    }
};

TEST_CASE("invocation scratch remains valid until invocation-end callbacks finish",
          "[evaluation][context][scratch][invocation_end_callback][feature:evaluation-context]") {
    invocation_scratch_state.value = nullptr;
    invocation_scratch_state.callback_count = 0;

    const auto source = node<invocation_scratch_source_definition>();
    run_graph([&] { (void)source(); });

    CHECK(invocation_scratch_state.callback_count == 1);
    CHECK(invocation_scratch_state.value == nullptr);
    invocation_scratch_state.registration.cancel();
}

struct section_scratch_callback_state {
    extension::InvocationEndRegistration registration;
    int *section_value{};
    int *parent_value{};
    int callback_count{};
};

section_scratch_callback_state section_scratch_state;

void inspect_section_scratch(void *receiver, EvaluationContext &) {
    auto &state = *static_cast<section_scratch_callback_state *>(receiver);
    CHECK(*state.section_value == 42);
    CHECK(*state.parent_value == 84);
    ++state.callback_count;
}

struct section_scratch_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        constexpr auto section_type = 0;
        auto section = details::EvaluationContextSection{nullptr, &section_type};
        {
            auto child = context->child(section);
            section_scratch_state.section_value =
                static_cast<int *>(child.scratch().allocate(sizeof(int), alignof(int)));
            *section_scratch_state.section_value = 42;
            child.add_invocation_end_callback(section_scratch_state.registration, &section_scratch_state,
                                              inspect_section_scratch);
        }

        section_scratch_state.parent_value =
            static_cast<int *>(context->scratch().allocate(sizeof(int), alignof(int)));
        *section_scratch_state.parent_value = 84;
        output.set(1);
    }
};

TEST_CASE("section contexts share invocation scratch and callbacks",
          "[evaluation][context][scratch][invocation_end_callback][feature:evaluation-context]") {
    section_scratch_state.section_value = nullptr;
    section_scratch_state.parent_value = nullptr;
    section_scratch_state.callback_count = 0;

    const auto source = node<section_scratch_source_definition>();
    run_graph([&] { (void)source(); });

    CHECK(section_scratch_state.callback_count == 1);
    section_scratch_state.registration.cancel();
}

struct scratch_reuse_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        allocation = context->scratch().allocate(sizeof(i64), alignof(i64));
        node_index = context->node_index();
        output.set(42);
    }

    inline static void *allocation{};
    inline static std::size_t node_index{};
};

struct scratch_reuse_sink_definition {
    In<TS<i64>> input;

    void evaluate(EvaluationContext *context) const {
        allocation = context->scratch().allocate(sizeof(i64), alignof(i64));
        node_index = context->node_index();
        value = input.value();
    }

    inline static void *allocation{};
    inline static std::size_t node_index{};
    inline static i64 value{};
};

TEST_CASE("evaluation contexts reset node scratch before publishing output",
          "[evaluation][context][scratch][feature:evaluation-context]") {
    const auto source = bg::node<scratch_reuse_source_definition>();

    const auto sink = bg::node<scratch_reuse_sink_definition>();
    scratch_reuse_source_definition::allocation = nullptr;
    scratch_reuse_sink_definition::allocation = nullptr;

    bg::run_graph([&] { sink(source()); });

    CHECK(scratch_reuse_sink_definition::value == 42);
    CHECK(scratch_reuse_sink_definition::allocation == scratch_reuse_source_definition::allocation);
    CHECK(scratch_reuse_sink_definition::node_index != scratch_reuse_source_definition::node_index);
}

struct invocation_end_callback_state {
    extension::InvocationEndRegistration registration;
    int callback_count{};
    int downstream_count{};
};

invocation_end_callback_state invocation_end_state;

void count_invocation_end_callback(void *receiver, EvaluationContext &) {
    ++*static_cast<int *>(receiver);
}

struct invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(42);
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
    }
};

struct invocation_end_start_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        output.set(42);
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
    }
};

struct invocation_end_sink_definition {
    In<TS<i64>> input;

    void evaluate() const {
        CHECK(invocation_end_state.callback_count == 1);
        ++invocation_end_state.downstream_count;
    }
};

struct invocation_end_nested_source_definition {
    In<TS<i64>> input;
    Out<TS<i64>> output;

    void evaluate(EvaluationContext *context) const {
        output.set(input.value());
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
    }
};

TEST_CASE("invocation-end callbacks deduplicate and run before output publication",
          "[evaluation][context][invocation_end_callback][feature:evaluation-context]") {
    invocation_end_state.callback_count = 0;
    invocation_end_state.downstream_count = 0;

    const auto source = node<invocation_end_source_definition>();
    const auto sink = node<invocation_end_sink_definition>();
    run_graph([&] { sink(source()); });

    CHECK(invocation_end_state.callback_count == 1);
    CHECK(invocation_end_state.downstream_count == 1);
}

TEST_CASE("invocation-end callbacks run after start functions and before output publication",
          "[evaluation][context][invocation_end_callback][lifecycle][feature:evaluation-context]") {
    invocation_end_state.callback_count = 0;
    invocation_end_state.downstream_count = 0;

    const auto source = node<invocation_end_start_source_definition>();
    const auto sink = node<invocation_end_sink_definition>();
    run_graph([&] { sink(source()); });

    CHECK(invocation_end_state.callback_count == 1);
    CHECK(invocation_end_state.downstream_count == 1);
}

struct invocation_end_start_failure_state {
    extension::InvocationEndRegistration registration;
    std::vector<int> events;
    int callback_count{};
    bool throw_next{};
};

invocation_end_start_failure_state invocation_end_start_failure;

void throw_from_start_invocation_end_callback(void *receiver, EvaluationContext &) {
    auto &state = *static_cast<invocation_end_start_failure_state *>(receiver);
    ++state.callback_count;
    state.events.push_back(2);
    if (std::exchange(state.throw_next, false)) {
        throw std::runtime_error{"start callback failed"};
    }
}

struct invocation_end_start_failure_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        invocation_end_start_failure.events.push_back(1);
        output.set(42);
        context->add_invocation_end_callback(invocation_end_start_failure.registration,
                                             &invocation_end_start_failure,
                                             throw_from_start_invocation_end_callback);
    }

    void stop() const noexcept { invocation_end_start_failure.events.push_back(-1); }
};

TEST_CASE("a completed start is stopped when its invocation-end callback throws",
          "[evaluation][context][invocation_end_callback][lifecycle][exception][feature:evaluation-context]") {
    invocation_end_start_failure.events.clear();
    invocation_end_start_failure.callback_count = 0;
    invocation_end_start_failure.throw_next = true;

    const auto source = node<invocation_end_start_failure_source_definition>();
    CHECK_THROWS_AS(run_graph([&] { (void)source(); }), std::runtime_error);
    CHECK(invocation_end_start_failure.events == std::vector{1, 2, -1});
    CHECK(invocation_end_start_failure.callback_count == 1);

    invocation_end_start_failure.events.clear();
    run_graph([&] { (void)source(); });
    CHECK(invocation_end_start_failure.events == std::vector{1, 2, -1});
    CHECK(invocation_end_start_failure.callback_count == 2);
    invocation_end_start_failure.registration.cancel();
}

TEST_CASE("nested graph node invocations own independent callback scopes",
          "[evaluation][context][invocation_end_callback][nested][feature:evaluation-context]") {
    invocation_end_state.callback_count = 0;

    const auto source = node<invocation_end_nested_source_definition>();
    const auto inner = graph("name"_n = "invocation_end_inner", [&](wp<TS<i64>> input) { return source(input); });
    const auto tested = [&](wp<TS<i64>> input, wp<TS<bool>> run) { return nested(inner, run, input); };
    const auto actual = testing::test_graph(tested, {1, 2}, {true, true});

    CHECK(actual.equals<i64>({1, 2}));
    CHECK(invocation_end_state.callback_count == 2);
    invocation_end_state.registration.cancel();
}

struct canceled_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(7);
        auto registration = extension::InvocationEndRegistration{};
        context->add_invocation_end_callback(registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
    }
};

TEST_CASE("destroying an invocation-end registration cancels its callback",
          "[evaluation][context][invocation_end_callback][feature:evaluation-context]") {
    invocation_end_state.callback_count = 0;

    const auto source = node<canceled_invocation_end_source_definition>();
    run_graph([&] { (void)source(); });

    CHECK(invocation_end_state.callback_count == 0);
}

struct pending_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    bool evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
        return false;
    }
};

struct successful_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_end_state.registration, &invocation_end_state.callback_count,
                                             count_invocation_end_callback);
    }
};

struct invocation_end_exception_state {
    extension::InvocationEndRegistration pending_registration;
    extension::InvocationEndRegistration throwing_registration;
    int callback_count{};
    bool throw_next{};
};

invocation_end_exception_state invocation_end_exception;

struct invocation_end_reregister_state {
    extension::InvocationEndRegistration registration;
    int callback_count{};
};

invocation_end_reregister_state invocation_end_reregister;

void count_or_throw_invocation_end_callback(void *receiver, EvaluationContext &) {
    auto &state = *static_cast<invocation_end_exception_state *>(receiver);
    ++state.callback_count;
    if (std::exchange(state.throw_next, false)) {
        throw std::runtime_error{"invocation-end callback failed"};
    }
}

void reregister_invocation_end_callback(void *receiver, EvaluationContext &context) {
    auto &state = *static_cast<invocation_end_reregister_state *>(receiver);
    ++state.callback_count;
    if (state.callback_count == 1) {
        context.add_invocation_end_callback(state.registration, &state, reregister_invocation_end_callback);
    }
}

struct reregister_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_end_reregister.registration, &invocation_end_reregister,
                                             reregister_invocation_end_callback);
    }
};

struct invocation_body_exception_state {
    extension::InvocationEndRegistration registration;
    int callback_count{};
};

invocation_body_exception_state invocation_body_exception;

void count_invocation_body_exception_callback(void *receiver, EvaluationContext &) {
    ++static_cast<invocation_body_exception_state *>(receiver)->callback_count;
}

struct throwing_invocation_body_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        context->add_invocation_end_callback(invocation_body_exception.registration, &invocation_body_exception,
                                             count_invocation_body_exception_callback);
        throw std::runtime_error{"node evaluation failed"};
    }
};

struct invocation_body_exception_reuse_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_body_exception.registration, &invocation_body_exception,
                                             count_invocation_body_exception_callback);
    }
};

struct throwing_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_end_exception.pending_registration, &invocation_end_exception,
                                             count_or_throw_invocation_end_callback);
        context->add_invocation_end_callback(invocation_end_exception.throwing_registration,
                                             &invocation_end_exception, count_or_throw_invocation_end_callback);
    }
};

struct reuse_invocation_end_source_definition {
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(EvaluationContext *context) const {
        output.set(1);
        context->add_invocation_end_callback(invocation_end_exception.pending_registration, &invocation_end_exception,
                                             count_or_throw_invocation_end_callback);
    }
};

TEST_CASE("a pending invocation cannot retain invocation-end callbacks",
          "[evaluation][context][invocation_end_callback][pending][feature:evaluation-context]") {
    invocation_end_state.callback_count = 0;

    const auto source = node<pending_invocation_end_source_definition>();
    auto message = std::string{};
    try {
        run_graph([&] { (void)source(); });
    } catch (const std::logic_error &error) {
        message = error.what();
    }
    CHECK(message.find("returned pending with invocation-end callbacks registered") != std::string::npos);

    const auto next_source = node<successful_invocation_end_source_definition>();
    run_graph([&] { (void)next_source(); });
    CHECK(invocation_end_state.callback_count == 1);

    invocation_end_state.registration.cancel();
}

TEST_CASE("a throwing invocation-end callback clears registrations that remain pending",
          "[evaluation][context][invocation_end_callback][exception][feature:evaluation-context]") {
    invocation_end_exception.callback_count = 0;
    invocation_end_exception.throw_next = true;

    const auto throwing_source = node<throwing_invocation_end_source_definition>();
    CHECK_THROWS_AS(run_graph([&] { throwing_source(); }), std::runtime_error);
    CHECK(invocation_end_exception.callback_count == 1);

    const auto reuse_source = node<reuse_invocation_end_source_definition>();
    run_graph([&] { (void)reuse_source(); });
    CHECK(invocation_end_exception.callback_count == 2);

    invocation_end_exception.pending_registration.cancel();
    invocation_end_exception.throwing_registration.cancel();
}

TEST_CASE("an invocation-end callback can register itself again while callbacks run",
          "[evaluation][context][invocation_end_callback][feature:evaluation-context]") {
    invocation_end_reregister.callback_count = 0;

    const auto source = node<reregister_invocation_end_source_definition>();
    run_graph([&] { (void)source(); });

    CHECK(invocation_end_reregister.callback_count == 2);
    invocation_end_reregister.registration.cancel();
}

TEST_CASE("a node exception clears its pending invocation-end callbacks",
          "[evaluation][context][invocation_end_callback][exception][feature:evaluation-context]") {
    invocation_body_exception.callback_count = 0;

    const auto throwing_source = node<throwing_invocation_body_source_definition>();
    CHECK_THROWS_AS(run_graph([&] { (void)throwing_source(); }), std::runtime_error);
    CHECK(invocation_body_exception.callback_count == 0);

    const auto reuse_source = node<invocation_body_exception_reuse_source_definition>();
    run_graph([&] { (void)reuse_source(); });
    CHECK(invocation_body_exception.callback_count == 1);
    invocation_body_exception.registration.cancel();
}

} // namespace
