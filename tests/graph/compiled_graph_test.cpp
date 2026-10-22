#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/ref/ref_input_storage.hpp>
#include <bg/time_series/ref/ref_notification.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace bg;

namespace {

struct source_definition {
    Out<TS<float>> output;

    void evaluate() const {}
};

struct integer_source_definition {
    Out<TS<i64>> output;

    void evaluate() const {}
};

class scale_template final : public NodeTemplate {
  public:
    [[nodiscard]] const Signature &default_signature() const override {
        static const auto input_type = util::static_storage<GenericTimeSeriesValueType>{
            float_type(), TimeSeriesGuarantee::ordinary};
        [[maybe_unused]] static const auto &input_type_registration = TypeRegistry::register_static(input_type.get());
        static const Signature value{
            util::symbol{"scale"},
            {{util::symbol{"input"}, &input_type.get()}, {util::symbol{"factor"}, &float_type()}},
            TS<float>::type()};

        return value;
    }

    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const CompiledNode &, void *, EvaluationContext *) { return true; }
};

} // namespace

TEST_CASE("compiled nodes keep hot data in node-local sections", "[compiled_graph][feature:graph-compilation]") {
    const auto source = node<source_definition>();
    const auto scale = scale_template{};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    wiring.finalize();
    CompiledGraph graph{wiring};

    REQUIRE(graph.node_count() == 2);
    const CompiledNode &source_node = graph.node(0);
    const CompiledNode &scale_node = graph.node(1);

    REQUIRE(source_node.subscribers().size() == 1);
    CHECK(source_node.subscribers()[0].node_index() == 1);
    CHECK(source_node.subscribers()[0].active());
    CHECK(source_node.subscribers()[0].required());
    CHECK_FALSE(source_node.subscribers()[0].active_optional());

    const auto parameters = scale_node.parameters();
    REQUIRE(parameters.size() == 2);
    CHECK(parameters[0].mode() == ParameterBindingMode::time_series);
    CHECK(parameters[0].last_updated_offset() == source_node.output_last_updated_offset());
    CHECK(parameters[0].value_offset() == source_node.output_value_offset());
    CHECK(parameters[1].mode() == ParameterBindingMode::scalar);
    CHECK(*static_cast<const float *>(parameters[1].scalar_data()) == 2.0F);

    const auto *node_address = reinterpret_cast<const std::byte *>(&scale_node);
    const auto *parameter_address = reinterpret_cast<const std::byte *>(parameters.data());
    const auto *scalar_address = static_cast<const std::byte *>(parameters[1].scalar_data());

    CHECK(parameter_address == node_address + sizeof(CompiledNode));
    CHECK(scalar_address == parameter_address + parameters.size() * sizeof(ParameterBinding));
    // The direct borrowed-input metadata offset adds 4 bytes to the header and 4 bytes of alignment padding.
    CHECK(graph.fixed_storage_size() == 252);
}

namespace {

struct runtime_signature_identity_definition {
    using output_declaration_type = Out<TS<i64>>;

    runtime_signature_identity_definition()
        : signature_{util::symbol{"runtime_identity"},
                     {{util::symbol{"input"}, &TS<i64>::type()}}, TS<i64>::type()} {}

    const Signature &signature() const noexcept { return signature_; }

    static void evaluate(NodeInvocation<>) {}

    Signature signature_;
};

} // namespace

TEST_CASE("typed callable nodes omit runtime-signature metadata",
          "[compiled_graph][feature:graph-compilation][definition:runtime]") {
    const auto typed_identity = node([](In<TS<i64>>, Out<TS<i64>>) {});

    const auto runtime_identity = node(runtime_signature_identity_definition{});
    const auto input_types = std::array<const bg::Type *, 1>{&TS<i64>::type()};
    const auto typed_signature = typed_identity.signature().resolve(input_types);
    const auto runtime_signature = runtime_identity.signature().resolve(input_types);

    CHECK_FALSE(typed_identity.uses_node_invocation_parameters());
    CHECK(runtime_identity.uses_node_invocation_parameters());
    CHECK_FALSE(typed_identity.uses_time_series_input_runtime_type(typed_signature, 0));
    CHECK(typed_identity.uses_output_runtime_type() == false);
    CHECK(runtime_identity.uses_time_series_input_runtime_type(runtime_signature, 0));
    CHECK(runtime_identity.uses_output_runtime_type());

    const auto compiled_sizes = [](const auto &transform) {
        auto wiring = GraphWiring{};

        const auto source = node<integer_source_definition>();
        {
            auto context = WiringContext{wiring};
            (void)transform(source());
        }
        wiring.finalize();
        const auto graph = CompiledGraph{wiring};
        return std::pair{graph.fixed_storage_size(), graph.runtime_storage_size()};
    };

    const auto typed_sizes = compiled_sizes(typed_identity);
    const auto runtime_sizes = compiled_sizes(runtime_identity);
    CHECK(typed_sizes.first < runtime_sizes.first);
    CHECK(typed_sizes.second == runtime_sizes.second);
}

namespace {

struct cycle_callback_node_definition {
    static constexpr bool uses_cycle_callbacks = true;

    Out<TS<i64>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("aggregate node declarations reserve cycle callback storage",
          "[compiled_graph][feature:graph-compilation][feature:lifecycle][definition:aggregate]") {
    const auto has_cycle_callbacks = [](const auto &node) {
        auto wiring = GraphWiring{};
        {
            auto context = WiringContext{wiring};
            (void)node();
        }
        wiring.finalize();
        const auto graph = CompiledGraph{wiring};
        return graph.has_cycle_callbacks();
    };

    CHECK(has_cycle_callbacks(node<cycle_callback_node_definition>()));
    CHECK_FALSE(has_cycle_callbacks(node<integer_source_definition>()));
}

TEST_CASE("capturing callables keep compiled data without parameter descriptors",
          "[compiled_graph][feature:graph-compilation][feature:lifecycle][definition:function][lifetime]") {
    const auto token = std::make_shared<int>(0);

    const auto source = node<integer_source_definition>();

    const auto copy = node([token](In<TS<i64>, dynamic_activity> input,
                                       Out<TS<i64>> output) {
        static_cast<void>(token);
        output.copy(input);
    });
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)copy(source());
    }
    wiring.finalize();

    const auto use_count_before_compilation = token.use_count();
    {
        const auto graph = CompiledGraph{wiring};
        CHECK(token.use_count() == use_count_before_compilation + 1);

        const auto &compiled_copy = graph.node(1);
        const auto *metadata = compiled_copy.node_invocation_metadata();
        REQUIRE(metadata != nullptr);
        CHECK(compiled_copy.controlled_inputs().size() == 1);
        CHECK(metadata->parameters(compiled_copy).empty());
        CHECK(metadata->controlled_input_indices(compiled_copy).empty());
        CHECK(metadata->compiled_data(compiled_copy) != nullptr);
    }
    CHECK(token.use_count() == use_count_before_compilation);
}

namespace {

struct failing_compiled_data_definition {
    using compiled_data_type = std::size_t;

    failing_compiled_data_definition(std::shared_ptr<int> token, std::shared_ptr<long> baseline,
                                     std::shared_ptr<bool> capture_was_constructed)
        : token_{std::move(token)}, baseline_{std::move(baseline)},
          capture_was_constructed_{std::move(capture_was_constructed)},
          signature_{util::symbol{"failing_compile"},
                     {{util::symbol{"input"}, &TS<i64>::type()}}, TS<i64>::type()} {}

    const Signature &signature() const noexcept { return signature_; }

    compiled_data_type compile(const ResolvedSignature &, allocator &) const {
        *capture_was_constructed_ = token_.use_count() > *baseline_;
        throw std::runtime_error{"requested compilation failure"};
    }

    static void evaluate(NodeInvocation<compiled_data_type>) {}

    std::shared_ptr<int> token_;
    std::shared_ptr<long> baseline_;
    std::shared_ptr<bool> capture_was_constructed_;
    Signature signature_;
};

} // namespace

TEST_CASE("compiled callable captures are destroyed when a later node fails compilation",
          "[compiled_graph][feature:graph-compilation][feature:lifecycle][definition:function][lifetime][failure]") {
    const auto token = std::make_shared<int>(0);
    const auto baseline = std::make_shared<long>();
    const auto capture_was_constructed = std::make_shared<bool>();

    const auto source = node<integer_source_definition>();

    const auto copy = node([token](In<TS<i64>> input, Out<TS<i64>> output) {
        static_cast<void>(token);
        output.copy(input);
    });

    const auto fail = node(failing_compiled_data_definition{token, baseline, capture_was_constructed});

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        (void)fail(copy(source()));
    }
    wiring.finalize();
    *baseline = token.use_count();

    CHECK_THROWS_WITH(CompiledGraph{wiring}, "requested compilation failure");
    CHECK(*capture_was_constructed);
    CHECK(token.use_count() == *baseline);
}

TEST_CASE("sampled reference bindings keep their mode and aligned metadata",
          "[compiled_graph][feature:reference][feature:sampling][binding:reference-to-value]") {
    struct dictionary_source {
        Out<TSD<i64, TS<i64>>> output;

        void evaluate() const {}
    };

    const auto source = node<dictionary_source>();

    const auto retain_reference = node([](In<REF<TSD<i64, TS<i64>>>> input,
                                              Out<REF<TSD<i64, TS<i64>>>> output) {
        output.copy(input);
    });

    const auto inspect = node([](In<TSD<i64, TS<i64>>> input,
                                     Out<TS<i64>> output) {
        if (input.valid()) {
            output.set(1);
        }
    });

    const auto inspect_dynamic = node([](In<TSD<i64, TS<i64>>,
                                                     dynamic_activity> input,
                                             Out<TS<i64>> output) {
        if (input.valid()) {
            output.set(1);
        }
    });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto reference = retain_reference(source());
        (void)inspect(reference);
        (void)inspect_dynamic(reference);
    }
    wiring.finalize();

    const auto graph = CompiledGraph{wiring};
    auto sampled_bindings = std::vector<const ParameterBinding *>{};
    for (auto node_index = std::size_t{}; node_index < graph.node_count(); ++node_index) {
        for (const auto &binding : graph.node(node_index).parameters()) {
            if (binding.mode() == ParameterBindingMode::reference && binding.has_reference_sample()) {
                sampled_bindings.push_back(&binding);
            }
        }
    }

    REQUIRE(sampled_bindings.size() == 2);
    CHECK(std::ranges::count_if(sampled_bindings, [](const auto *binding) {
              return binding->reference_dynamic_activity();
          }) == 1);
    for (const auto *binding : sampled_bindings) {
        CHECK(binding->value_offset() % alignof(ReferenceInputStorage) == 0);
    }
}

TEST_CASE("all_valid metadata follows an empty runtime-type array at pointer alignment",
          "[compiled_graph][feature:validity][validity:all-valid][layout]") {
    struct source_definition {
        Out<TS<i64>> output;

        void start() const { output.set(1); }
    };
    struct stateful_gate_definition {
        State<i64> evaluations;
        In<TS<i64>, all_valid> input;
        Out<TS<i64>> output;

        void evaluate() const { output.set(++*evaluations); }
    };

    const auto source = node<source_definition>();

    const auto gate = node<stateful_gate_definition>();
    const auto gate_signature = gate.signature().resolve(std::array<const bg::Type *, 1>{&TS<i64>::type()});
    CHECK_FALSE(gate.uses_time_series_input_runtime_type(gate_signature, 0));
    CHECK_FALSE(gate.uses_output_runtime_type());

    const auto increment = node([](i64 input) { return input + 1; });

    const auto double_value = node([](i64 input) { return input * 2; });

    const auto combine = node([](In<TS<i64>> left, In<TS<i64>> right,
                                     Out<TS<i64>> output) { output.set(*left + *right); });

    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto value = gate(source());
        static_cast<void>(combine(increment(value), double_value(value)));
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    const CompiledNode *stateful_gate{};
    for (auto index = std::size_t{}; index < graph.node_count(); ++index) {
        const auto &node = graph.node(index);
        if (node.has_state() && node.has_all_valid_input_metadata()) {
            stateful_gate = &node;
            break;
        }
    }

    REQUIRE(stateful_gate != nullptr);
    REQUIRE(stateful_gate->subscribers().size() == 2);
    const auto descriptors = stateful_gate->all_valid_input_descriptors();
    REQUIRE(descriptors.size() == 1);
    CHECK(descriptors[0].parameter_index == 0);
    CHECK(descriptors[0].declared_type == &TS<i64>::type());
}

namespace {

struct input_node_index_sink {
    static constexpr bool uses_input_node_indices = true;

    In<TS<float>> first;
    In<TS<float>> second;

    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled node input producer indices use compiled node order",
          "[feature:graph-compilation][series:value][shape:chain]") {
    const auto source = node<source_definition>();

    const auto sink = node<input_node_index_sink>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto input = source();
        sink(input, input);
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    CHECK(graph.node(1).input_node_index(0) == 0);
    CHECK(graph.node(1).input_node_index(1) == 0);
    CHECK_FALSE(graph.node(0).input_node_index(0).has_value());

    struct ordinary_two_input_sink {
        In<TS<float>> first;
        In<TS<float>> second;

        void evaluate() const {}
    };

    const auto ordinary_sink = node<ordinary_two_input_sink>();
    auto ordinary_wiring = GraphWiring{};
    {
        auto context = WiringContext{ordinary_wiring};
        const auto input = source();
        ordinary_sink(input, input);
    }
    ordinary_wiring.finalize();
    const auto ordinary_graph = CompiledGraph{ordinary_wiring};
    CHECK(graph.fixed_storage_size() == ordinary_graph.fixed_storage_size() + 2 * sizeof(std::uint32_t));
}

namespace {

struct all_valid_input_node_index_sink {
    static constexpr bool uses_input_node_indices = true;

    In<TS<i64>, all_valid> input;

    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled nodes retain all_valid metadata with input producer indices",
          "[feature:graph-compilation][feature:validity][series:value][scalar:i64][shape:chain]") {
    const auto source = node<integer_source_definition>();

    const auto sink = node<all_valid_input_node_index_sink>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(source());
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    const auto &compiled_sink = graph.node(1);
    CHECK(compiled_sink.input_node_index(0) == 0);
    CHECK(compiled_sink.has_all_valid_input_metadata());

    const auto descriptors = compiled_sink.all_valid_input_descriptors();
    REQUIRE(descriptors.size() == 1);
    CHECK(descriptors[0].parameter_index == 0);
    CHECK(descriptors[0].declared_type == &TS<i64>::type());
}

namespace {

struct runtime_valid_items_all_valid_definition {
    runtime_valid_items_all_valid_definition()
        : signature_{util::symbol{"runtime_valid_items_all_valid"},
                     {Parameter{.name = util::symbol{"input"},
                                    .type = &TSD<i64, TS<i64>>::type(),
                                    .validity = InputValidity::all_valid,
                                    .valid_items = true}}} {}

    const Signature &signature() const noexcept { return signature_; }
    static void evaluate(NodeInvocation<>) {}

    Signature signature_;
};

} // namespace

TEST_CASE("compiled all_valid descriptors omit valid_items dictionary checks",
          "[compiled_graph][feature:validity][validity:all-valid][input-guarantee:valid-items][series:tsd]") {
    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto sink = node(runtime_valid_items_all_valid_definition{});
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(source());
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    const auto &compiled_sink = graph.node(1);
    CHECK_FALSE(compiled_sink.has_all_valid_input_metadata());
    CHECK(compiled_sink.all_valid_input_descriptors().empty());
}

TEST_CASE("compiled node input producer indices omit lowered inputs and remap direct inputs",
          "[feature:graph-compilation][feature:constant][series:value][shape:chain][guarantee:const]") {
    using namespace operators;

    const auto source = node<source_definition>();

    const auto sink = node<input_node_index_sink>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        const auto constant = const_(1.0F);
        sink(constant, source());
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    const auto &compiled_sink = graph.node(1);
    CHECK_FALSE(compiled_sink.input_node_index(0).has_value());
    CHECK(compiled_sink.input_node_index(1) == 0);
}

TEST_CASE("compiled nodes store one activity record for each controlled input",
          "[feature:input-activity][feature:graph-compilation][series:value][definition:aggregate]"
          "[shape:chain][activity:active]") {
    struct controlled_sink_definition {
        In<TS<float>, dynamic_activity> input;
        void evaluate() const { input.make_passive(); }
    };

    const auto source = node<source_definition>();

    const auto sink = node<controlled_sink_definition>();
    auto wiring = GraphWiring{};
    {
        auto context = WiringContext{wiring};
        sink(source());
    }

    wiring.finalize();
    const auto compiled = CompiledGraph{wiring};
    CHECK(compiled.node(0).subscribers().empty());
    CHECK(compiled.node(0).output_notification() != nullptr);
    CHECK(compiled.node(1).controlled_inputs().size() == 1);
    CHECK(sizeof(Subscriber) == sizeof(std::uint32_t));
    CHECK(sizeof(ControlledInput) == sizeof(std::uint32_t));
}

namespace {

struct reference_subscription_source {
    Out<TS<i64>> output;
    void evaluate() const {}
};

struct reference_subscription_pass {
    In<REF<TS<i64>>> input;
    Out<REF<TS<i64>>> output;
    void evaluate() const { output.copy(input); }
};

struct two_reference_input_sink {
    In<TS<i64>> left;
    In<TS<i64>> right;
    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled reference notifications identify each receiving input slot",
          "[compiled_graph][subscriptions][feature:graph-compilation]") {
    const auto source = node<reference_subscription_source>();

    const auto pass = node<reference_subscription_pass>();

    const auto sink = node<two_reference_input_sink>();
    auto wiring = GraphWiring{};

    {
        const auto context = WiringContext{wiring};
        const auto reference = pass(source());
        sink(reference, reference);
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    const auto &reference_node = graph.node(2);
    const auto *notification = reference_node.output_notification();
    REQUIRE(notification != nullptr);
    const auto bytes = notification->records();
    const auto reference_subscribers = std::span{
        reinterpret_cast<const details::ReferenceInputSubscriber *>(bytes.data()),
        bytes.size() / sizeof(details::ReferenceInputSubscriber),
    };

    const auto sink_parameters = graph.node(3).parameters();

    REQUIRE(reference_subscribers.size() == 2);
    CHECK(reference_subscribers[0].node_index() == 3);
    CHECK(reference_subscribers[0].input_offset() == sink_parameters[0].value_offset());
    CHECK(reference_subscribers[1].node_index() == 3);
    CHECK(reference_subscribers[1].input_offset() == sink_parameters[1].value_offset());

    const auto node_subscribers = reference_node.subscribers();
    REQUIRE(node_subscribers.size() == 1);
    CHECK(node_subscribers[0].node_index() == 3);
    CHECK(node_subscribers[0].active());
    CHECK_FALSE(node_subscribers[0].required());
    CHECK_FALSE(node_subscribers[0].active_optional());
}

TEST_CASE("graph wiring can compile more than once", "[compiled_graph][feature:graph-compilation]") {
    const auto source = node<source_definition>();
    const auto scale = scale_template{};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    wiring.finalize();
    CompiledGraph first{wiring};
    CompiledGraph second{wiring};

    const void *first_scalar = first.node(1).parameters()[1].scalar_data();
    const void *second_scalar = second.node(1).parameters()[1].scalar_data();

    CHECK(first_scalar != second_scalar);
    CHECK(*static_cast<const float *>(first_scalar) == 2.0F);
    CHECK(*static_cast<const float *>(second_scalar) == 2.0F);
}

namespace {

struct passive_copy_definition {
    In<TS<float>, passive, optional> input;
    Out<TS<float>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("compiled parameters retain validity without a passive subscription",
          "[compiled_graph][feature:graph-compilation]") {
    const auto source = node<source_definition>();

    const auto passive_copy = node<passive_copy_definition>();
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        (void)passive_copy(source());
    }

    wiring.finalize();
    CompiledGraph graph{wiring};

    CHECK(graph.node(0).subscribers().empty());
    CHECK(graph.node(1).parameters()[0].validity() == optional);
}

TEST_CASE("a compiled graph owns its scalar copies", "[compiled_graph][feature:graph-compilation]") {
    const auto source = node<source_definition>();
    const auto scale = scale_template{};
    std::unique_ptr<CompiledGraph> graph;

    {
        GraphWiring wiring;
        WiringContext context{wiring};
        (void)scale(source(), 2.0F);
        wiring.finalize();
        graph = std::make_unique<CompiledGraph>(wiring);
    }

    const ParameterBinding &factor = graph->node(1).parameters()[1];
    CHECK(*static_cast<const float *>(factor.scalar_data()) == 2.0F);
}

namespace {

class counting_memory_resource final : public std::pmr::memory_resource {
  public:
    [[nodiscard]] std::size_t allocation_count() const noexcept { return allocation_count_; }

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocation_count_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void *data, std::size_t bytes, std::size_t alignment) override {
        --allocation_count_;
        std::pmr::new_delete_resource()->deallocate(data, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }

    std::size_t allocation_count_{};
};

} // namespace

TEST_CASE("compiled fixed storage uses one allocation", "[compiled_graph][feature:graph-compilation][storage:fixed]") {
    const auto source = node<source_definition>();
    const auto scale = scale_template{};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    counting_memory_resource memory;

    {
        wiring.finalize();
        CompiledGraph graph{wiring, allocator{memory}};

        CHECK(graph.fixed_storage_size() > 0);
        CHECK(memory.allocation_count() == 1);
    }

    CHECK(memory.allocation_count() == 0);
}

namespace {

struct evaluation_time_source_definition {
    Out<TS<EvaluationTime>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("a compiled graph stores an evaluation time output", "[compiled_graph][feature:graph-compilation]") {
    const auto evaluation_time_source = node<evaluation_time_source_definition>();
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        (void)evaluation_time_source();
    }

    wiring.finalize();
    const CompiledGraph graph{wiring};

    REQUIRE(graph.node_count() == 1);
    CHECK(graph.node(0).has_output());
    CHECK(graph.node(0).output_last_updated_offset() != graph.node(0).output_value_offset());
}

namespace {

struct private_data {
    explicit private_data(int source) : value(source) {}
    private_data(const private_data &) = default;
    private_data &operator=(const private_data &) = delete;

    int value;
};

struct opaque_sink_definition {
    inline static private_data *observed{};
    inline static int observed_value{};

    Scalar<Opaque<private_data *>> pointer;
    Scalar<Opaque<private_data>> object;

    void evaluate() const {
        observed = *pointer;
        observed_value = object->value;
    }
};

struct unsupported_opaque_output {
    Out<TS<Opaque<private_data *>>> output;

    void evaluate() const {}
};

static_assert(AggregateNodeClass<opaque_sink_definition>);
static_assert(!AggregateNodeClass<unsupported_opaque_output>);

} // namespace

TEST_CASE("a compiled graph owns an opaque scalar copy", "[compiled_graph][feature:graph-compilation]") {
    const auto opaque_sink = node<opaque_sink_definition>();
    auto value = private_data{42};
    const auto pointer = ScalarValue<Opaque<private_data *>>{&value};
    const auto object = ScalarValue<Opaque<private_data>>{value};
    GraphWiring wiring;

    {
        WiringContext context{wiring};
        opaque_sink(pointer, object);
    }

    wiring.finalize();
    const CompiledGraph graph{wiring};
    const auto *stored_pointer = static_cast<private_data *const *>(graph.node(0).parameters()[0].scalar_data());
    const auto *stored_object = static_cast<const private_data *>(graph.node(0).parameters()[1].scalar_data());
    auto runtime_global_state = GlobalState{};
    RuntimeGraph runtime{graph, runtime_global_state};

    opaque_sink_definition::observed = nullptr;
    opaque_sink_definition::observed_value = 0;
    runtime.start(MIN_DT + std::chrono::microseconds{1});
    runtime.schedule(0);
    runtime.evaluate(MIN_DT + std::chrono::microseconds{1});

    CHECK(*stored_pointer == &value);
    CHECK(stored_object->value == 42);
    CHECK(opaque_sink_definition::observed == &value);
    CHECK(opaque_sink_definition::observed_value == 42);
}
