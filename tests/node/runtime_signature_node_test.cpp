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

namespace {

struct alignas(std::max_align_t) PaddedScalarValue {
    bg::i64 value{};
    bg::i64 padding{};
};

class SelectedOutputRuntimeType final : public bg::TimeSeriesValueRuntimeType {
  public:
    SelectedOutputRuntimeType(const bg::TimeSeriesType &type, bg::i64 layout)
        : TimeSeriesValueRuntimeType{type, bg::cpp_scalar_runtime_type<bg::i64>(), value_size(layout),
                                    value_alignment(layout), false, false, true}, layout_{layout} {}

    void construct_initial_value(void *destination) const noexcept override {
        if (layout_ == 1) {
            std::construct_at(static_cast<bg::i64 *>(destination));
        } else {
            std::construct_at(static_cast<PaddedScalarValue *>(destination));
        }
    }

    void destroy_value(void *value, bg::allocator &) const noexcept override {
        if (layout_ == 1) {
            std::destroy_at(static_cast<bg::i64 *>(value));
        } else {
            std::destroy_at(static_cast<PaddedScalarValue *>(value));
        }
    }

  private:
    bg::i64 layout_;

    static std::size_t value_size(bg::i64 layout) noexcept {
        return layout == 1 ? sizeof(bg::i64) : sizeof(PaddedScalarValue);
    }

    static std::size_t value_alignment(bg::i64 layout) noexcept {
        return layout == 1 ? alignof(bg::i64) : alignof(PaddedScalarValue);
    }
};

const bg::TimeSeriesRuntimeType &selected_output_runtime_type(const bg::TimeSeriesType &type,
                                                               bg::i64 layout) {
    static auto runtime_types =
        std::map<std::pair<const bg::TimeSeriesType *, bg::i64>, std::unique_ptr<SelectedOutputRuntimeType>>{};
    const auto key = std::pair{&type, layout};
    const auto [entry, inserted] = runtime_types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<SelectedOutputRuntimeType>(type, layout);
    }
    return *entry->second;
}

using CALLABLE_SCALAR = bg::ScalarVariable<"CALLABLE_SCALAR">;

struct valid_output_runtime_type_callback {
    const bg::TimeSeriesRuntimeType &operator()(const bg::WiringArguments &) const;
};

struct invalid_output_runtime_type_callback {
    bg::i64 operator()(const bg::WiringArguments &) const;
};

template <typename Function>
concept can_make_output_runtime_type_option = requires(Function &&function) {
    bg::output_runtime_type = std::forward<Function>(function);
};

using OutputRuntimeTypeOption = bg::details::CallableOutputRuntimeType<valid_output_runtime_type_callback>;

static_assert(bg::details::callable_node_options<OutputRuntimeTypeOption>);
static_assert(!bg::details::callable_node_options<OutputRuntimeTypeOption, OutputRuntimeTypeOption>);
static_assert(can_make_output_runtime_type_option<valid_output_runtime_type_callback>);
static_assert(!can_make_output_runtime_type_option<invalid_output_runtime_type_callback>);

struct InstanceOutputRuntimeDefinition {
    explicit InstanceOutputRuntimeDefinition(bg::i64 layout)
        : layout_{layout},
          signature_{bg::util::symbol{"instance_output_runtime"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const { return signature_; }

    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &arguments) const {
        ++selector_calls;
        CHECK(arguments.size() == 1);
        CHECK(&arguments.resolved_type(0) == &bg::TS<bg::i64>::type());
        return selected_output_runtime_type(*arguments.resolved_output_type(), layout_);
    }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto input = invocation.input<bg::In<bg::TS<bg::i64>>>(0);
        const auto output = invocation.output_as<bg::Out<bg::TS<bg::i64>>>();
        CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type(), expected_layout));
        output.set(input.value());
        CHECK(output.value() == input.value());
    }

    inline static int selector_calls{};
    static constexpr bg::i64 expected_layout = 2;

  private:
    bg::i64 layout_;
    bg::Signature signature_;
};

template <template <typename, auto...> typename Field> struct RuntimeValidityBaseBundle {
    Field<bg::i64> value;
};

template <template <typename, auto...> typename Field>
struct RuntimeValidityDerivedBundle : bg::bundle_extends<RuntimeValidityBaseBundle, Field> {
    Field<bg::i64> extra;
};

struct RuntimeNodeState {
    bg::i64 evaluations{};
};

struct RuntimeNodeData {
    bg::i64 factor;
};

struct RuntimeNodeDefinition {
    using compiled_data_type = RuntimeNodeData;
    using state_type = RuntimeNodeState;

    RuntimeNodeDefinition()
        : signature_{bg::util::symbol{"runtime_node"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}},
                     bg::TS<bg::i64>::type()} {}

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
                return bg::scalar_cast<bg::i64>(input->value_view()).value();
            },
            ddv::noop);
        const auto result = number * invocation.compiled_data().factor + ++invocation.state().evaluations;
        const auto scalar = bg::Scalar<bg::i64>::from(result);
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(scalar); }, ddv::noop);
    }

    bg::Signature signature_;
};

struct InstanceTimedSchedulingDefinition {
    explicit InstanceTimedSchedulingDefinition(bool uses_timed_scheduling)
        : uses_timed_scheduling_{uses_timed_scheduling},
          signature_{bg::util::symbol{"instance_timed_scheduling"}, {}} {}

    const bg::Signature &signature() const { return signature_; }
    bool uses_timed_scheduling() const noexcept { return uses_timed_scheduling_; }
    static void evaluate(bg::NodeInvocation<>) {}

  private:
    bool uses_timed_scheduling_;
    bg::Signature signature_;
};

struct StaticTimedSchedulingDefinition {
    static constexpr bool uses_timed_scheduling = true;

    StaticTimedSchedulingDefinition() : signature_{bg::util::symbol{"static_timed_scheduling"}, {}} {}

    const bg::Signature &signature() const { return signature_; }
    static void evaluate(bg::NodeInvocation<>) {}

  private:
    bg::Signature signature_;
};

TEST_CASE("a runtime-signature node exposes resolved parameters, compiled data, and state",
          "[node][runtime-signature][definition:runtime][feature:node-api][declaration:runtime]") {
    const auto runtime_node = bg::node(RuntimeNodeDefinition{});

    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*runtime_node(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3}).equals({3, 6, 9}));
}

TEST_CASE("runtime-signature node timed scheduling can depend on the definition instance",
          "[node][runtime-signature][feature:scheduling]") {
    const auto clock_only = bg::RuntimeSignatureNodeTemplate<InstanceTimedSchedulingDefinition>{
        InstanceTimedSchedulingDefinition{false}};
    const auto scheduled = bg::RuntimeSignatureNodeTemplate<InstanceTimedSchedulingDefinition>{
        InstanceTimedSchedulingDefinition{true}};
    const auto static_scheduled =
        bg::RuntimeSignatureNodeTemplate<StaticTimedSchedulingDefinition>{StaticTimedSchedulingDefinition{}};

    CHECK_FALSE(clock_only.uses_timed_scheduling());
    CHECK(scheduled.uses_timed_scheduling());
    CHECK(static_scheduled.uses_timed_scheduling());
}

TEST_CASE("a callable selects its output runtime type from wiring arguments",
          "[feature:node-api][series:value][declaration:generic][definition:function]") {
    auto selector_calls = 0;
    const auto compute = bg::node(
        bg::eval = [](bg::In<bg::TS<CALLABLE_SCALAR>> input, bg::i64 layout,
                      bg::Out<bg::TS<CALLABLE_SCALAR>> output) {
            CHECK(&output.runtime_type() ==
                  &selected_output_runtime_type(output.runtime_type().type(), layout));
            output.set_value(input.value_view());
        },
        bg::output_runtime_type = [&selector_calls](const bg::WiringArguments &arguments)
            -> const bg::TimeSeriesRuntimeType & {
            ++selector_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(&arguments.resolved_type(0) == &bg::TS<bg::i64>::type());
            CHECK(&arguments.resolved_type(1) == &bg::int64_type());
            const auto layout = bg::scalar_cast<bg::i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });

    const auto run = [&](bg::i64 layout) {
        const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
            return bg::wp<bg::TS<bg::i64>>{compute(input, layout)};
        };
        return bg::testing::test_graph(graph, std::array<bg::i64, 2>{4, 5});
    };

    CHECK(run(1).equals<bg::i64>({4, 5}));
    CHECK(selector_calls == 1);
    CHECK(run(2).equals<bg::i64>({4, 5}));
    CHECK(selector_calls == 2);
}

TEST_CASE("a concrete callable evaluates using its selected output runtime type",
          "[feature:node-api][series:value][declaration:concrete][definition:function]") {
    auto selector_calls = 0;
    const auto compute = bg::node(
        bg::eval = [](bg::In<bg::TS<bg::i64>> input, bg::i64 layout, bg::Out<bg::TS<bg::i64>> output) {
            CHECK(&output.runtime_type() == &selected_output_runtime_type(output.runtime_type().type(), layout));
            output.set(input.value());
            CHECK(output.value() == input.value());
        },
        bg::output_runtime_type = [&selector_calls](const bg::WiringArguments &arguments)
            -> const bg::TimeSeriesRuntimeType & {
            ++selector_calls;
            REQUIRE(arguments.size() == 2);
            CHECK(&arguments.resolved_type(0) == &bg::TS<bg::i64>::type());
            CHECK(&arguments.resolved_type(1) == &bg::int64_type());
            const auto layout = bg::scalar_cast<bg::i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });

    const auto run = [&](bg::i64 layout) {
        const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
            return bg::wp<bg::TS<bg::i64>>{compute(input, layout)};
        };
        return bg::testing::test_graph(graph, std::array<bg::i64, 2>{4, 5});
    };

    CHECK(run(1).equals<bg::i64>({4, 5}));
    CHECK(selector_calls == 1);
    CHECK(run(2).equals<bg::i64>({4, 5}));
    CHECK(selector_calls == 2);
}

TEST_CASE("operator cache hits pass current scalar arguments to output runtime hooks",
          "[feature:operator][feature:node-api][feature:wiring][series:value][definition:function]") {
    auto requirement_calls = 0;
    auto selector_calls = 0;
    auto operation = bg::operator_<auto(bg::TS<bg::i64>, bg::i64) -> bg::TS<bg::i64>>{
        "runtime_layout", {"input", "layout"}};
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<CALLABLE_SCALAR>> input, bg::i64,
                      bg::Out<bg::TS<CALLABLE_SCALAR>> output) { output.set_value(input.value_view()); },
        bg::require = [&](const bg::WiringArguments &arguments) {
            ++requirement_calls;
            return &arguments.resolved_type(1) == &bg::int64_type();
        },
        bg::output_runtime_type = [&](const bg::WiringArguments &arguments)
            -> const bg::TimeSeriesRuntimeType & {
            ++selector_calls;
            const auto layout = bg::scalar_cast<bg::i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        }));

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    {
        const bg::WiringContext context{wiring};
        const auto input = source();
        const auto first = static_cast<bg::WiringPoint>(operation(input, 1));
        const auto second = static_cast<bg::WiringPoint>(operation(input, 2));

        CHECK(first.has_output());
        CHECK(second.has_output());
        CHECK(first.record() != second.record());
    }

    CHECK(selector_calls == 2);
    CHECK(requirement_calls == 1);
    CHECK_NOTHROW(wiring.finalize());
    REQUIRE(wiring.outputs().size() == 3);
    CHECK(wiring.outputs()[1].runtime_type ==
          &selected_output_runtime_type(bg::TS<bg::i64>::type(), 1));
    CHECK(wiring.outputs()[2].runtime_type ==
          &selected_output_runtime_type(bg::TS<bg::i64>::type(), 2));
}

TEST_CASE("a callable's repeated identical invocation reuses its output record",
          "[feature:node-api][feature:wiring][series:value][definition:function]") {
    auto selector_calls = 0;
    const auto compute = bg::node(
        bg::eval = [](bg::In<bg::TS<CALLABLE_SCALAR>> input, bg::i64,
                      bg::Out<bg::TS<CALLABLE_SCALAR>> output) { output.set_value(input.value_view()); },
        bg::output_runtime_type = [&](const bg::WiringArguments &arguments)
            -> const bg::TimeSeriesRuntimeType & {
            ++selector_calls;
            const auto layout = bg::scalar_cast<bg::i64>(*arguments.scalar(1)).value();
            return selected_output_runtime_type(*arguments.resolved_output_type(), layout);
        });
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    auto first = bg::WiringPoint{};
    auto repeated = bg::WiringPoint{};
    {
        const bg::WiringContext context{wiring};
        const auto input = source();
        first = static_cast<bg::WiringPoint>(compute(input, 1));
        repeated = static_cast<bg::WiringPoint>(compute(input, 1));
    }

    CHECK(selector_calls == 2);
    CHECK_NOTHROW(wiring.finalize());
    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].runtime_type == &selected_output_runtime_type(bg::TS<bg::i64>::type(), 1));
    CHECK(wiring.output_index(first) == wiring.output_index(repeated));
    CHECK(wiring.output_node_index(first) == wiring.output_node_index(repeated));
}

TEST_CASE("a callable without an output runtime hook uses the declaration runtime type",
          "[feature:node-api][series:value][declaration:concrete][definition:function]") {
    const auto copy = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input);
    });
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    auto output = bg::WiringPoint{};
    {
        const bg::WiringContext context{wiring};
        output = static_cast<bg::WiringPoint>(copy(source()));
    }

    wiring.finalize();
    REQUIRE(wiring.outputs().size() == 2);
    CHECK(wiring.outputs()[1].runtime_type == &bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>());
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK_FALSE(compiled.node(wiring.output_node_index(output)).has_output_runtime_type());
}

TEST_CASE("a runtime-signature definition can select an output runtime type from its instance",
          "[feature:node-api][series:value][declaration:runtime][definition:runtime]") {
    InstanceOutputRuntimeDefinition::selector_calls = 0;
    const auto node = bg::node(InstanceOutputRuntimeDefinition{InstanceOutputRuntimeDefinition::expected_layout});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*node(input)};
    };

    CHECK(bg::testing::test_graph(graph, std::array<bg::i64, 3>{1, 2, 3}).equals<bg::i64>({1, 2, 3}));
    CHECK(InstanceOutputRuntimeDefinition::selector_calls == 1);
}

TEST_CASE("graph wiring owns a temporary runtime-signature node template",
          "[node][runtime-signature][definition:runtime][lifetime][feature:node-api]"
          "[declaration:runtime]") {
    const auto graph = [](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*bg::node(RuntimeNodeDefinition{})(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({3, 6}));
}

struct RuntimeSinkDefinition {
    RuntimeSinkDefinition()
        : signature_{bg::util::symbol{"runtime_sink"}, {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()}}} {}

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
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        CHECK_FALSE(sink(input).has_value());
        return input;
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, 2}));
}

struct RuntimeDynamicActivityDefinition {
    RuntimeDynamicActivityDefinition()
        : signature_{bg::util::symbol{"runtime_dynamic_activity"},
                     {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type(), bg::active, bg::required, false,
                       bg::details::VariadicMode::none, nullptr, false, true}},
                     bg::TS<bg::i64>::type()} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        auto input = invocation.template input<bg::In<bg::TS<bg::i64>, bg::dynamic_activity>>(0);
        input.make_passive();
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(input.value_view()); },
                                  ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("a runtime-signature input changes its activity",
          "[feature:input-activity][series:value][definition:runtime][activity:active][transition:update]") {
    const auto dynamic = bg::node(RuntimeDynamicActivityDefinition{});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*dynamic(input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2}).equals({1, std::nullopt}));
}

struct OptionalScalarDefinition {
    OptionalScalarDefinition()
        : signature_{
              bg::util::symbol{"optional_scalar"},
              {{bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()},
               {bg::util::symbol{"fallback"}, &bg::cpp_scalar_type<bg::i64>(), bg::active, bg::required, true}},
              bg::TS<bg::i64>::type()} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void evaluate(bg::NodeInvocation<> invocation) {
        const auto scalar = *invocation.scalars().begin();
        CHECK(scalar.parameter_index() == 1);
        CHECK(scalar.parameter_name() == bg::util::symbol{"fallback"});

        const auto input = (*invocation.inputs().begin()).value();
        const auto number = *input.visit(
            [](const bg::In<bg::TS<bg::SCALAR>> *value) {
                return bg::scalar_cast<bg::i64>(value->value_view()).value();
            },
            ddv::noop);
        const auto result = scalar.has_value() ? -1 : number;
        const auto value = bg::Scalar<bg::i64>::from(result);
        invocation.output().visit([&](bg::Out<bg::TS<bg::SCALAR>> *output) { output->set_value(value); }, ddv::noop);
    }

    bg::Signature signature_;
};

TEST_CASE("an absent optional scalar remains enumerable",
          "[node][runtime-signature][definition:runtime][scalar][feature:node-api]") {
    const auto runtime_node = bg::node(OptionalScalarDefinition{});

    const auto graph = [&](bg::wp<bg::TS<bg::i64>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*runtime_node(input, std::optional<bg::i64>{})};
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

TEST_CASE("a runtime-signature node applies all_valid using invocation metadata",
          "[node][runtime-signature][definition:runtime][feature:validity][validity:all-valid]"
          "[series:fixed-tsl][scalar:i64]") {
    const auto runtime_node = bg::node(RuntimeAllValidDefinition{});
    const auto graph = [&](bg::wp<bg::TS<bg::i64>> known,
                           bg::wp<bg::TSL<bg::TS<bg::i64>, 2>> input) -> bg::wp<bg::TS<bg::i64>> {
        return bg::wp<bg::TS<bg::i64>>{*runtime_node(known, bg::i64{7}, input)};
    };

    CHECK(bg::testing::test_graph(graph, {1, 2, 3},
                                  {{{0, 10}, {1, 20}}, {{1, literal::INVALID}}, {{1, 22}}})
              .equals({7, std::nullopt, 7}));
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
    const auto graph = [&](bg::wp<bg::TSB<RuntimeValidityDerivedBundle>> input) {
        return bg::wp<bg::TS<bg::i64>>{*runtime_node(input)};
    };

    CHECK(bg::testing::test_graph(graph, {{{"value", 42}}}).equals({1}));
}

struct RuntimeDictionarySourceDefinition {
    RuntimeDictionarySourceDefinition()
        : output_type_{&bg::TSD<bg::i64, bg::TS<bg::i64>>::type()},
          signature_{bg::util::symbol{"runtime_dictionary_source"}, {}, *output_type_} {}

    [[nodiscard]] const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        auto output = invocation.output();
        const auto key = bg::ScalarValue<bg::i64>{1};
        const auto value = bg::ScalarValue<bg::i64>{42};
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
    bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input;

    void evaluate() const { value = *input[1]; }

    inline static bg::i64 value{};
};

TEST_CASE("a runtime-signature node constructs runtime-defined dictionary storage",
          "[node][runtime-signature][definition:runtime][tsd][series:tsd][feature:node-api]"
          "[declaration:runtime]") {
    const auto source = bg::node(RuntimeDictionarySourceDefinition{});
    const auto sink = bg::node<RuntimeDictionarySinkDefinition>();
    RuntimeDictionarySinkDefinition::value = 0;

    bg::run_graph([&] { sink(bg::wp<bg::TSD<bg::i64, bg::TS<bg::i64>>>{*source()}); });

    CHECK(RuntimeDictionarySinkDefinition::value == 42);
}

} // namespace
