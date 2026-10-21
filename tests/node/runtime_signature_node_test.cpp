#include <bg/graph.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

using namespace bg;

namespace {

struct alignas(std::max_align_t) PaddedScalarValue {
    i64 value{};
    i64 padding{};
};

class SelectedOutputRuntimeType final : public TimeSeriesValueRuntimeType {
  public:
    SelectedOutputRuntimeType(const TimeSeriesType &type, i64 layout)
        : TimeSeriesValueRuntimeType{type, cpp_scalar_runtime_type<i64>(), value_size(layout),
                                    value_alignment(layout), false, false, true}, layout_{layout} {}

    void construct_initial_value(void *destination) const noexcept override {
        if (layout_ == 1) {
            std::construct_at(static_cast<i64 *>(destination));
        } else {
            std::construct_at(static_cast<PaddedScalarValue *>(destination));
        }
    }

    void destroy_value(void *value, allocator &) const noexcept override {
        if (layout_ == 1) {
            std::destroy_at(static_cast<i64 *>(value));
        } else {
            std::destroy_at(static_cast<PaddedScalarValue *>(value));
        }
    }

  private:
    i64 layout_;

    static std::size_t value_size(i64 layout) noexcept {
        return layout == 1 ? sizeof(i64) : sizeof(PaddedScalarValue);
    }

    static std::size_t value_alignment(i64 layout) noexcept {
        return layout == 1 ? alignof(i64) : alignof(PaddedScalarValue);
    }
};

const TimeSeriesRuntimeType &selected_output_runtime_type(const TimeSeriesType &type,
                                                               i64 layout) {
    static auto runtime_types =
        std::map<std::pair<const TimeSeriesType *, i64>, std::unique_ptr<SelectedOutputRuntimeType>>{};
    const auto key = std::pair{&type, layout};
    const auto [entry, inserted] = runtime_types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<SelectedOutputRuntimeType>(type, layout);
    }
    return *entry->second;
}

using CALLABLE_SCALAR = ScalarVariable<"CALLABLE_SCALAR">;

struct valid_output_runtime_type_callback {
    const TimeSeriesRuntimeType &operator()(const WiringArguments &) const;
};

struct invalid_output_runtime_type_callback {
    i64 operator()(const WiringArguments &) const;
};

template <typename Function>
concept can_make_output_runtime_type_option = requires(Function &&function) {
    output_runtime_type = std::forward<Function>(function);
};

using OutputRuntimeTypeOption = details::CallableOutputRuntimeType<valid_output_runtime_type_callback>;

static_assert(details::callable_node_options<OutputRuntimeTypeOption>);
static_assert(!details::callable_node_options<OutputRuntimeTypeOption, OutputRuntimeTypeOption>);
static_assert(can_make_output_runtime_type_option<valid_output_runtime_type_callback>);
static_assert(!can_make_output_runtime_type_option<invalid_output_runtime_type_callback>);

struct InstanceOutputRuntimeDefinition {
    explicit InstanceOutputRuntimeDefinition(i64 layout)
        : layout_{layout},
          signature_{util::symbol{"instance_output_runtime"},
                     {{util::symbol{"input"}, &TS<i64>::type()}}, TS<i64>::type()} {}

    const Signature &signature() const { return signature_; }

    const TimeSeriesRuntimeType &output_runtime_type(const WiringArguments &arguments) const {
        ++selector_calls;
        CHECK(arguments.size() == 1);
        CHECK(&arguments.resolved_type(0) == &TS<i64>::type());
        return selected_output_runtime_type(*arguments.resolved_output_type(), layout_);
    }

    static void evaluate(NodeInvocation<> invocation) {
        const auto input = invocation.input<In<TS<i64>>>(0);
        const auto output = invocation.output_as<Out<TS<i64>>>();
        CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type(), expected_layout));
        output.set(input.value());
        CHECK(output.value() == input.value());
    }

    inline static int selector_calls{};
    static constexpr i64 expected_layout = 2;

  private:
    i64 layout_;
    Signature signature_;
};

template <template <typename, auto...> typename Field> struct RuntimeValidityBaseBundle {
    Field<i64> value;
};

template <template <typename, auto...> typename Field>
struct RuntimeValidityDerivedBundle : bundle_extends<RuntimeValidityBaseBundle, Field> {
    Field<i64> extra;
};

struct RuntimeNodeState {
    i64 evaluations{};
};

struct RuntimeNodeData {
    i64 factor;
};

struct RuntimeNodeDefinition {
    using compiled_data_type = RuntimeNodeData;
    using state_type = RuntimeNodeState;

    RuntimeNodeDefinition()
        : signature_{util::symbol{"runtime_node"},
                     {{util::symbol{"input"}, &TS<i64>::type()}},
                     TS<i64>::type()} {}

    [[nodiscard]] const Signature &signature() const { return signature_; }

    [[nodiscard]] RuntimeNodeData compile(const ResolvedSignature &, allocator &) const {
        return {.factor = 2};
    }

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        const auto input = *invocation.inputs().begin();
        CHECK(input.parameter_index() == 0);
        CHECK(input.parameter_name() == util::symbol{"input"});
        CHECK(input.parameter_type().is_time_series());

        const auto value = input.value();
        const auto number = *value.visit(
            [](const In<TS<SCALAR>> *input) {
                return scalar_cast<i64>(input->value_view()).value();
            },
            ddv::noop);
        const auto result = number * invocation.compiled_data().factor + ++invocation.state().evaluations;
        const auto scalar = Scalar<i64>::from(result);
        invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(scalar); }, ddv::noop);
    }

    Signature signature_;
};

struct InstanceTimedSchedulingDefinition {
    explicit InstanceTimedSchedulingDefinition(bool uses_timed_scheduling)
        : uses_timed_scheduling_{uses_timed_scheduling},
          signature_{util::symbol{"instance_timed_scheduling"}, {}} {}

    const Signature &signature() const { return signature_; }
    bool uses_timed_scheduling() const noexcept { return uses_timed_scheduling_; }
    static void evaluate(NodeInvocation<>) {}

  private:
    bool uses_timed_scheduling_;
    Signature signature_;
};

struct StaticTimedSchedulingDefinition {
    static constexpr bool uses_timed_scheduling = true;

    StaticTimedSchedulingDefinition() : signature_{util::symbol{"static_timed_scheduling"}, {}} {}

    const Signature &signature() const { return signature_; }
    static void evaluate(NodeInvocation<>) {}

  private:
    Signature signature_;
};

TEST_CASE("a runtime-signature node exposes resolved parameters, compiled data, and state",
          "[node][runtime-signature][definition:runtime][feature:node-api][declaration:runtime]") {
    const auto runtime_node = node(RuntimeNodeDefinition{});

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*runtime_node(input)};
    };

    CHECK(testing::test_graph(graph, {1, 2, 3}).equals({3, 6, 9}));
}

TEST_CASE("runtime-signature node timed scheduling can depend on the definition instance",
          "[node][runtime-signature][feature:scheduling]") {
    const auto clock_only = RuntimeSignatureNodeTemplate<InstanceTimedSchedulingDefinition>{
        InstanceTimedSchedulingDefinition{false}};
    const auto scheduled = RuntimeSignatureNodeTemplate<InstanceTimedSchedulingDefinition>{
        InstanceTimedSchedulingDefinition{true}};
    const auto static_scheduled =
        RuntimeSignatureNodeTemplate<StaticTimedSchedulingDefinition>{StaticTimedSchedulingDefinition{}};

    CHECK_FALSE(clock_only.uses_timed_scheduling());
    CHECK(scheduled.uses_timed_scheduling());
    CHECK(static_scheduled.uses_timed_scheduling());
}

TEST_CASE("a callable selects its output runtime type from wiring arguments",
          "[feature:node-api][series:value][declaration:generic][definition:function]") {
    auto selector_calls = 0;
    const auto compute = node(
        eval = [](In<TS<CALLABLE_SCALAR>> input, i64 layout,
                      Out<TS<CALLABLE_SCALAR>> output) {
            CHECK(&output.runtime_type() ==
                  &selected_output_runtime_type(output.runtime_type().type(), layout));
            output.set_value(input.value_view());
        },
        output_runtime_type = [&selector_calls](const WiringArguments &arguments)
            -> const TimeSeriesRuntimeType & {
            ++selector_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(&arguments.resolved_type(0) == &TS<i64>::type());
            CHECK(&arguments.resolved_type(1) == &int64_type());
            const auto layout = scalar_cast<i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });

    const auto run = [&](i64 layout) {
        const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
            return wp<TS<i64>>{compute(input, layout)};
        };
        return testing::test_graph(graph, std::array<i64, 2>{4, 5});
    };

    CHECK(run(1).equals<i64>({4, 5}));
    CHECK(selector_calls == 1);
    CHECK(run(2).equals<i64>({4, 5}));
    CHECK(selector_calls == 2);
}

TEST_CASE("a concrete callable evaluates using its selected output runtime type",
          "[feature:node-api][series:value][declaration:concrete][definition:function]") {
    auto selector_calls = 0;
    const auto compute = node(
        eval = [](In<TS<i64>> input, i64 layout, Out<TS<i64>> output) {
            CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type(), layout));
            output.set(input.value());
            CHECK(output.value() == input.value());
        },
        output_runtime_type = [&selector_calls](const WiringArguments &arguments)
            -> const TimeSeriesRuntimeType & {
            ++selector_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(&arguments.resolved_type(0) == &TS<i64>::type());
            CHECK(&arguments.resolved_type(1) == &int64_type());
            const auto layout = scalar_cast<i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });

    const auto run = [&](i64 layout) {
        const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
            return wp<TS<i64>>{compute(input, layout)};
        };
        return testing::test_graph(graph, std::array<i64, 2>{4, 5});
    };

    CHECK(run(1).equals<i64>({4, 5}));
    CHECK(selector_calls == 1);
    CHECK(run(2).equals<i64>({4, 5}));
    CHECK(selector_calls == 2);
}

TEST_CASE("operator cache hits pass current scalar arguments to output runtime hooks",
          "[feature:operator][feature:node-api][feature:wiring][series:value][definition:function]") {
    auto requirement_calls = 0;
    auto selector_calls = 0;
    auto operation = operator_<auto(TS<i64>, i64) -> TS<i64>>{
        "runtime_layout", {"input", "layout"}};
    operation.overload(node(
        eval = [](In<TS<CALLABLE_SCALAR>> input, i64,
                      Out<TS<CALLABLE_SCALAR>> output) { output.set_value(input.value_view()); },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            return &arguments.resolved_type(1) == &int64_type();
        },
        output_runtime_type = [&](const WiringArguments &arguments)
            -> const TimeSeriesRuntimeType & {
            ++selector_calls;
            const auto layout = scalar_cast<i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        }));

    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    {
        const WiringContext context{wiring};
        const auto input = source();
        const auto first = static_cast<WiringPoint>(operation(input, 1));
        const auto second = static_cast<WiringPoint>(operation(input, 2));

        CHECK(first.has_output());
        CHECK(second.has_output());
        CHECK(first.record() != second.record());
    }

    CHECK(selector_calls == 2);
    CHECK(requirement_calls == 1);
    CHECK_NOTHROW(wiring.finalize());
    REQUIRE(wiring.outputs().size() == 3);
    CHECK(wiring.outputs()[1].runtime_type ==
          &selected_output_runtime_type(TS<i64>::type(), 1));
    CHECK(wiring.outputs()[2].runtime_type ==
          &selected_output_runtime_type(TS<i64>::type(), 2));
}

TEST_CASE("a callable's repeated identical invocation reuses its output record",
          "[feature:node-api][feature:wiring][series:value][definition:function]") {
    auto selector_calls = 0;
    const auto compute = node(
        eval = [](In<TS<CALLABLE_SCALAR>> input, i64,
                      Out<TS<CALLABLE_SCALAR>> output) { output.set_value(input.value_view()); },
        output_runtime_type = [&](const WiringArguments &arguments)
            -> const TimeSeriesRuntimeType & {
            ++selector_calls;
            const auto layout = scalar_cast<i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });
    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    auto first = WiringPoint{};
    auto repeated = WiringPoint{};
    {
        const WiringContext context{wiring};
        const auto input = source();
        first = static_cast<WiringPoint>(compute(input, 1));
        repeated = static_cast<WiringPoint>(compute(input, 1));
    }

    CHECK(selector_calls == 2);
    CHECK_NOTHROW(wiring.finalize());
    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].runtime_type == &selected_output_runtime_type(TS<i64>::type(), 1));
    CHECK(wiring.output_index(first) == wiring.output_index(repeated));
    CHECK(wiring.output_node_index(first) == wiring.output_node_index(repeated));
}

TEST_CASE("a callable without an output runtime hook uses the declaration runtime type",
          "[feature:node-api][series:value][declaration:concrete][definition:function]") {
    const auto copy = node([](In<TS<i64>> input, Out<TS<i64>> output) {
        output.set(*input);
    });
    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    auto output = WiringPoint{};
    {
        const WiringContext context{wiring};
        output = static_cast<WiringPoint>(copy(source()));
    }

    wiring.finalize();
    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].runtime_type == &cpp_time_series_runtime_type<TS<i64>>());
    const auto compiled = CompiledGraph{wiring};
    CHECK_FALSE(compiled.node(wiring.output_node_index(output)).has_output_runtime_type());
}

TEST_CASE("a runtime-signature definition can select an output runtime type from its instance",
          "[feature:node-api][series:value][declaration:runtime][definition:runtime]") {
    InstanceOutputRuntimeDefinition::selector_calls = 0;
    const auto node = bg::node(InstanceOutputRuntimeDefinition{InstanceOutputRuntimeDefinition::expected_layout});

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*node(input)};
    };

    CHECK(testing::test_graph(graph, std::array<i64, 3>{1, 2, 3}).equals<i64>({1, 2, 3}));
    CHECK(InstanceOutputRuntimeDefinition::selector_calls == 1);
}

TEST_CASE("graph wiring owns a temporary runtime-signature node template",
          "[node][runtime-signature][definition:runtime][lifetime][feature:node-api]"
          "[declaration:runtime]") {
    const auto graph = [](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*node(RuntimeNodeDefinition{})(input)};
    };

    CHECK(testing::test_graph(graph, {1, 2}).equals({3, 6}));
}

struct RuntimeSinkDefinition {
    RuntimeSinkDefinition()
        : signature_{util::symbol{"runtime_sink"}, {{util::symbol{"input"}, &TS<i64>::type()}}} {}

    [[nodiscard]] const Signature &signature() const { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        CHECK(invocation.inputs().begin() != invocation.inputs().end());
    }

    Signature signature_;
};

struct EmptyInvocationSourceDefinition {
    static constexpr bool schedule_on_start = true;

    EmptyInvocationSourceDefinition()
        : signature_{util::symbol{"empty_invocation_source"}, {}, TS<i64>::type()} {}

    const Signature &signature() const noexcept { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        CHECK(invocation.parameter_count() == 0);
        CHECK(invocation.inputs().begin() == invocation.inputs().end());
        CHECK(invocation.scalars().begin() == invocation.scalars().end());
        invocation.output().visit([](Out<TS<SCALAR>> *output) {
            const auto value = i64{1};
            output->set_value(Scalar<i64>::from(value));
        }, ddv::noop);
    }

    Signature signature_;
};

TEST_CASE("a runtime-signature node with no parameters exposes empty input and scalar ranges",
          "[node][runtime-signature][definition:runtime][feature:node-api]") {
    const auto source = node(EmptyInvocationSourceDefinition{});
    CHECK(testing::test_graph(source).equals<i64>({1}));
}

TEST_CASE("a runtime-signature node can omit its output",
          "[node][runtime-signature][definition:runtime][output][feature:node-api][declaration:runtime]") {
    const auto sink = node(RuntimeSinkDefinition{});

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        CHECK_FALSE(sink(input).has_value());
        return input;
    };

    CHECK(testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeDynamicActivityDefinition {
    RuntimeDynamicActivityDefinition()
        : signature_{util::symbol{"runtime_dynamic_activity"},
                     {{util::symbol{"input"}, &TS<i64>::type(), active, required, false,
                       details::VariadicMode::none, nullptr, false, true}},
                     TS<i64>::type()} {}

    [[nodiscard]] const Signature &signature() const { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        auto input = invocation.template input<In<TS<i64>, dynamic_activity>>(0);
        input.make_passive();
        invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(input.value_view()); },
                                  ddv::noop);
    }

    Signature signature_;
};

TEST_CASE("a runtime-signature input changes its activity",
          "[feature:input-activity][series:value][definition:runtime][activity:active][transition:update]") {
    const auto dynamic = node(RuntimeDynamicActivityDefinition{});

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*dynamic(input)};
    };

    CHECK(testing::test_graph(graph, {1, 2}).equals({1, std::nullopt}));
}

struct OptionalScalarDefinition {
    OptionalScalarDefinition()
        : signature_{
              util::symbol{"optional_scalar"},
              {{util::symbol{"input"}, &TS<i64>::type()},
               {util::symbol{"fallback"}, &cpp_scalar_type<i64>(), active, required, true}},
              TS<i64>::type()} {}

    [[nodiscard]] const Signature &signature() const { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        const auto scalar = *invocation.scalars().begin();
        CHECK(scalar.parameter_index() == 1);
        CHECK(scalar.parameter_name() == util::symbol{"fallback"});

        const auto input = (*invocation.inputs().begin()).value();
        const auto number = *input.visit(
            [](const In<TS<SCALAR>> *value) {
                return scalar_cast<i64>(value->value_view()).value();
            },
            ddv::noop);
        const auto result = scalar.has_value() ? -1 : number;
        const auto value = Scalar<i64>::from(result);
        invocation.output().visit([&](Out<TS<SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    Signature signature_;
};

TEST_CASE("an absent optional scalar remains enumerable",
          "[node][runtime-signature][definition:runtime][scalar][feature:node-api]") {
    const auto runtime_node = node(OptionalScalarDefinition{});

    const auto graph = [&](wp<TS<i64>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*runtime_node(input, std::optional<i64>{})};
    };

    CHECK(testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeAllValidDefinition {
    RuntimeAllValidDefinition()
        : signature_{util::symbol{"runtime_all_valid"},
                     {{util::symbol{"known"}, &TS<i64>::type()},
                      {util::symbol{"adjustment"}, &cpp_scalar_type<i64>()},
                      {util::symbol{"items"}, &TSL<TS<i64>, 2>::type(), active,
                       all_valid}},
                     TS<i64>::type()} {}

    const Signature &signature() const { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        auto known_is_valid = false;
        auto items_are_valid = false;
        for (const auto input : invocation.inputs()) {
            if (input.parameter_index() == 0) {
                known_is_valid = input.value().valid();
            } else if (input.parameter_index() == 2) {
                items_are_valid = input.value().all_valid(
                    type_cast<TimeSeriesType>(input.parameter_type()));
            }
        }

        const auto adjustment = *invocation.scalars().begin();
        CHECK(adjustment.parameter_index() == 1);
        const auto output_value = known_is_valid && items_are_valid
                                      ? scalar_cast<i64>(adjustment.value()).value()
                                      : i64{};
        const auto value = Scalar<i64>::from(output_value);
        invocation.output().visit(
            [&](Out<TS<SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    Signature signature_;
};

TEST_CASE("a runtime-signature node applies all_valid using invocation metadata",
          "[node][runtime-signature][definition:runtime][feature:validity][validity:all-valid]"
          "[series:fixed-tsl][scalar:i64]") {
    const auto runtime_node = node(RuntimeAllValidDefinition{});

    const auto graph = [&](wp<TS<i64>> known,
                           wp<TSL<TS<i64>, 2>> input) -> wp<TS<i64>> {
        return wp<TS<i64>>{*runtime_node(known, i64{7}, input)};
    };

    CHECK(testing::test_graph(graph, {1, 2, 3},
                                  {{{0, 10}, {1, 20}}, {{1, literal::INVALID}}, {{1, 22}}})
              .equals({7, std::nullopt, 7}));
}

struct RuntimeBundleAllValidDefinition {
    RuntimeBundleAllValidDefinition()
        : signature_{util::symbol{"runtime_bundle_all_valid"},
                     {{util::symbol{"bundle"}, &TSB<RuntimeValidityBaseBundle>::type(), active,
                       all_valid}},
                     TS<i64>::type()} {}

    const Signature &signature() const { return signature_; }

    static void evaluate(NodeInvocation<> invocation) {
        const auto input = *invocation.inputs().begin();
        const auto &declared_type = type_cast<TimeSeriesType>(input.parameter_type());
        const auto result = input.value().all_valid(declared_type) ? i64{1} : i64{};
        const auto value = Scalar<i64>::from(result);
        invocation.output().visit(
            [&](Out<TS<SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    Signature signature_;
};

TEST_CASE("a runtime-signature input checks the declared base bundle schema",
          "[node][runtime-signature][definition:runtime][feature:validity][validity:all-valid]"
          "[series:tsb][schema:named][binding:covariant][representation:native]") {
    const auto runtime_node = node(RuntimeBundleAllValidDefinition{});

    const auto graph = [&](wp<TSB<RuntimeValidityDerivedBundle>> input) {
        return wp<TS<i64>>{*runtime_node(input)};
    };

    CHECK(testing::test_graph(graph, {{{"value", 42}}}).equals({1}));
}

struct RuntimeDictionarySourceDefinition {
    RuntimeDictionarySourceDefinition()
        : output_type_{&TSD<i64, TS<i64>>::type()},
          signature_{util::symbol{"runtime_dictionary_source"}, {}, *output_type_} {}

    [[nodiscard]] const Signature &signature() const { return signature_; }

    static void start(NodeInvocation<> invocation) {
        auto output = invocation.output();
        const auto key = ScalarValue<i64>{1};
        const auto value = ScalarValue<i64>{42};
        output.visit(
            [&](Out<TSD<SCALAR, TS_T>> *dictionary) {
                auto item = (*dictionary)[Scalar<SCALAR>::from(key)];
                item.visit([&](Out<TS<SCALAR>> *item_output) { item_output->set_value(value.view()); },
                           ddv::noop);
            },
            ddv::noop);
    }

    const TimeSeriesDictionaryType *output_type_;
    Signature signature_;
};

struct RuntimeDictionarySinkDefinition {
    In<TSD<i64, TS<i64>>> input;

    void evaluate() const { value = *input[1]; }

    inline static i64 value{};
};

TEST_CASE("a runtime-signature node constructs runtime-defined dictionary storage",
          "[node][runtime-signature][definition:runtime][tsd][series:tsd][feature:node-api]"
          "[declaration:runtime]") {
    const auto source = node(RuntimeDictionarySourceDefinition{});

    const auto sink = node<RuntimeDictionarySinkDefinition>();
    RuntimeDictionarySinkDefinition::value = 0;

    run_graph([&] { sink(wp<TSD<i64, TS<i64>>>{*source()}); });

    CHECK(RuntimeDictionarySinkDefinition::value == 42);
}

} // namespace
