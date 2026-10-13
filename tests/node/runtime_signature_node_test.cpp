#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace {

template <template <typename, auto...> typename Field> struct RuntimeValidityBaseBundle {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field>
struct RuntimeValidityDerivedBundle : bg::bundle_extends<RuntimeValidityBaseBundle, Field> {
    Field<bg::i64> extra;
};

struct RuntimeValidityDerivedSource {
    bg::Out<bg::TSB<RuntimeValidityDerivedBundle>> output;

    void start() const { output->value.set(42); }
};

struct RuntimeNodeState {
    std::int64_t evaluations{};
};

struct RuntimeNodeData {
    std::int64_t factor;
};

struct RuntimeNodeDefinition {
    using compiled_data_type = RuntimeNodeData;
    using state_type = RuntimeNodeState;

    RuntimeNodeDefinition()
        : signature_{bg::util::symbol{"runtime_node"},
                     {{bg::util::symbol{"input"}, &bg::TS<std::int64_t>::type()}},
                     bg::TS<std::int64_t>::type()} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    [[nodiscard]] RuntimeNodeData compile(const bg::ResolvedSignature &, bg::allocator &) const {
        return {.factor = 2};
    }

    static void evaluate(bg::NodeInvocation<compiled_data_type, state_type> invocation) {
        const auto input = *invocation.inputs().begin();
        CHECK(input.parameter_index() == 0);
        CHECK(input.parameter_name() == bg::util::symbol{"input"});
        CHECK(input.parameter_type().is_time_series());

        const auto value = input.value();
        const auto number = *value.visit(
            [](const bg::In<bg::TS<bg::SCALAR>> *input) {
                return bg::scalar_cast<std::int64_t>(input->value_view()).value();
            },
            ddv::noop);
        const auto result = number * invocation.compiled_data().factor + ++invocation.state().evaluations;
        const auto scalar = bg::Scalar<std::int64_t>::from(result);
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(scalar); }, ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature node exposes resolved parameters, compiled data, and state",
          "[node][runtime-signature][definition:runtime][feature:node-api][declaration:runtime]") {
    const auto runtime_node = bg::node(RuntimeNodeDefinition{});

    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*runtime_node(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({3, 6, 9}));
}

TEST_CASE("graph wiring owns a temporary runtime-signature node template",
          "[node][runtime-signature][definition:runtime][lifetime][feature:node-api]"
          "[declaration:runtime]") {
    const auto graph = [](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*bg::node(RuntimeNodeDefinition{})(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({3, 6}));
}

struct RuntimeSinkDefinition {
    RuntimeSinkDefinition()
        : signature_{bg::util::symbol{"runtime_sink"}, {{bg::util::symbol{"input"}, &bg::TS<std::int64_t>::type()}}} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        CHECK(invocation.inputs().begin() != invocation.inputs().end());
    }

    bg::Signature signature_;
};

struct EmptyInvocationSourceDefinition {
    static constexpr bool schedule_on_start = true;

    EmptyInvocationSourceDefinition()
        : signature_{bg::util::symbol{"empty_invocation_source"}, {}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        CHECK(invocation.parameter_count() == 0);
        CHECK(invocation.inputs().begin() == invocation.inputs().end());
        CHECK(invocation.scalars().begin() == invocation.scalars().end());
        invocation.output().visit([](bg::Out<bg::TS<bg::SCALAR>> *output) {
            const auto value = bg::i64{1};
            output->set_value(bg::Scalar<bg::i64>::from(value));
        }, ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature node with no parameters exposes empty input and scalar ranges",
          "[node][runtime-signature][definition:runtime][feature:node-api]") {
    const auto source = bg::node(EmptyInvocationSourceDefinition{});
    CHECK(bg::testing::test_graph(source).equals<bg::i64>({1}));
}

TEST_CASE("a runtime-signature node can omit its output",
          "[node][runtime-signature][definition:runtime][output][feature:node-api][declaration:runtime]") {
    const auto sink = bg::node(RuntimeSinkDefinition{});
    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        CHECK_FALSE(sink(input).has_value());
        return input;
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeDynamicActivityDefinition {
    RuntimeDynamicActivityDefinition()
        : signature_{bg::util::symbol{"runtime_dynamic_activity"},
                     {{bg::util::symbol{"input"}, &bg::TS<std::int64_t>::type(), bg::active, bg::required, false,
                       bg::details::VariadicMode::none, nullptr, false, true}},
                     bg::TS<std::int64_t>::type()} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        auto input = invocation.template input<bg::In<bg::TS<std::int64_t>, bg::dynamic_activity>>(0);
        input.make_passive();
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(input.value_view()); },
                                  ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature input changes its activity",
          "[feature:input-activity][series:value][definition:runtime][activity:active][transition:update]") {
    const auto dynamic = bg::node(RuntimeDynamicActivityDefinition{});
    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*dynamic(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, std::nullopt}));
}

struct OptionalScalarDefinition {
    OptionalScalarDefinition()
        : signature_{
              bg::util::symbol{"optional_scalar"},
              {{bg::util::symbol{"input"}, &bg::TS<std::int64_t>::type()},
               {bg::util::symbol{"fallback"}, &bg::cpp_scalar_type<std::int64_t>(), bg::active, bg::required, true}},
              bg::TS<std::int64_t>::type()} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto scalar = *invocation.scalars().begin();
        CHECK(scalar.parameter_index() == 1);
        CHECK(scalar.parameter_name() == bg::util::symbol{"fallback"});

        const auto input = (*invocation.inputs().begin()).value();
        const auto number = *input.visit(
            [](const bg::In<bg::TS<bg::SCALAR>> *value) {
                return bg::scalar_cast<std::int64_t>(value->value_view()).value();
            },
            ddv::noop);
        const auto result = scalar.has_value() ? -1 : number;
        const auto value = bg::Scalar<std::int64_t>::from(result);
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("an absent optional scalar remains enumerable",
          "[node][runtime-signature][definition:runtime][scalar][feature:node-api]") {
    const auto runtime_node = bg::node(OptionalScalarDefinition{});

    const auto graph = [&](bg::wp<bg::TS<std::int64_t>> input) -> bg::wp<bg::TS<std::int64_t>> {
        return bg::wp<bg::TS<std::int64_t>>{*runtime_node(input, std::optional<std::int64_t>{})};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeAllValidDefinition {
    RuntimeAllValidDefinition()
        : signature_{bg::util::symbol{"runtime_all_valid"},
                     {{bg::util::symbol{"known"}, &bg::TS<bg::i64>::type()},
                      {bg::util::symbol{"adjustment"}, &bg::cpp_scalar_type<bg::i64>()},
                      {bg::util::symbol{"items"}, &bg::TSL<bg::TS<bg::i64>, 2>::type(), bg::active,
                       bg::all_valid}},
                     bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        auto known_is_valid = false;
        auto items_are_valid = false;
        for (const auto input : invocation.inputs()) {
            if (input.parameter_index() == 0) {
                known_is_valid = input.value().valid();
            } else if (input.parameter_index() == 2) {
                items_are_valid = input.value().all_valid(
                    bg::type_cast<bg::TimeSeriesType>(input.parameter_type()));
            }
        }

        const auto adjustment = *invocation.scalars().begin();
        CHECK(adjustment.parameter_index() == 1);
        const auto output_value = known_is_valid && items_are_valid
                                      ? bg::scalar_cast<bg::i64>(adjustment.value()).value()
                                      : bg::i64{};
        const auto value = bg::Scalar<bg::i64>::from(output_value);
        invocation.output().visit(
            [&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    bg::Signature signature_;
};

struct RuntimePartiallyValidListDefinition {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TSL<bg::TS<bg::i64>, 2>> output;

    void start(bg::EvaluationContext *context) const {
        output[0].set(10);
        output[1].set(20);
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        if (*step == 0) {
            output[1].invalidate();
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        } else {
            output[1].set(22);
        }

        ++*step;
    }
};

TEST_CASE("a runtime-signature node applies all_valid using invocation metadata",
          "[node][runtime-signature][definition:runtime][feature:validity][validity:all-valid]"
          "[series:fixed-tsl][scalar:i64]") {
    const auto runtime_node = bg::node(RuntimeAllValidDefinition{});
    const auto list_source = bg::node<RuntimePartiallyValidListDefinition>();
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> known) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*runtime_node(known, bg::i64{7}, list_source())};
    };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 3>{1, 2, 3}).equals({7, std::nullopt, 7}));
}

struct RuntimeBundleAllValidDefinition {
    RuntimeBundleAllValidDefinition()
        : signature_{bg::util::symbol{"runtime_bundle_all_valid"},
                     {{bg::util::symbol{"bundle"}, &bg::TSB<RuntimeValidityBaseBundle>::type(), bg::active,
                       bg::all_valid}},
                     bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto input = *invocation.inputs().begin();
        const auto &declared_type = bg::type_cast<bg::TimeSeriesType>(input.parameter_type());
        const auto result = input.value().all_valid(declared_type) ? bg::i64{1} : bg::i64{};
        const auto value = bg::Scalar<bg::i64>::from(result);
        invocation.output().visit(
            [&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature input checks the declared base bundle schema",
          "[node][runtime-signature][definition:runtime][feature:validity][validity:all-valid]"
          "[series:tsb][schema:named][binding:covariant][representation:native]") {
    const auto runtime_node = bg::node(RuntimeBundleAllValidDefinition{});
    const auto source = bg::node<RuntimeValidityDerivedSource>();
    const auto graph = [&] { return bg::wp<bg::TS<bg::i64>>{*runtime_node(source())}; };

    CHECK(bg::testing::test_graph(graph).equals({1}));
}

struct RuntimeDictionarySourceDefinition {
    RuntimeDictionarySourceDefinition()
        : output_type_{&bg::TSD<std::int64_t, bg::TS<std::int64_t>>::type()},
          signature_{bg::util::symbol{"runtime_dictionary_source"}, {}, *output_type_} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        auto output = invocation.output();
        const auto key = bg::ScalarValue<std::int64_t>{1};
        const auto value = bg::ScalarValue<std::int64_t>{42};
        output.visit(
            [&](bg::Out<bg::TSD<bg::SCALAR, bg::TS_T>> *dictionary) {
                auto item = (*dictionary)[bg::Scalar<bg::SCALAR>::from(key)];
                item.visit([&](bg::Out<bg::TS<bg::SCALAR>> *item_output) { item_output->set_value(value.view()); },
                           ddv::noop);
            },
            ddv::noop);
    }

    const bg::TimeSeriesDictionaryType *output_type_;
    bg::Signature signature_;
};

struct RuntimeDictionarySinkDefinition {
    bg::In<bg::TSD<std::int64_t, bg::TS<std::int64_t>>> input;

    void evaluate() const { value = *input[1]; }

    inline static std::int64_t value{};
};

TEST_CASE("a runtime-signature node constructs runtime-defined dictionary storage",
          "[node][runtime-signature][definition:runtime][tsd][series:tsd][feature:node-api]"
          "[declaration:runtime]") {
    const auto source = bg::node(RuntimeDictionarySourceDefinition{});
    const auto sink = bg::node<RuntimeDictionarySinkDefinition>();
    RuntimeDictionarySinkDefinition::value = 0;

    bg::run_graph([&] { sink(bg::wp<bg::TSD<std::int64_t, bg::TS<std::int64_t>>>{*source()}); });

    CHECK(RuntimeDictionarySinkDefinition::value == 42);
}

} // namespace
