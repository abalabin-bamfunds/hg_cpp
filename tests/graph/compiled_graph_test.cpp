#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/ref/ref_notification.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>

namespace {

struct source_definition {
    bg::Out<bg::TS<float>> output;

    void evaluate() const {}
};

struct input_node_index_sink {
    static constexpr bool uses_input_node_indices = true;

    bg::In<bg::TS<float>> first;
    bg::In<bg::TS<float>> second;

    void evaluate() const {}
};

struct integer_source_definition {
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {}
};

struct all_valid_input_node_index_sink {
    static constexpr bool uses_input_node_indices = true;

    bg::In<bg::TS<bg::i64>, bg::all_valid> input;

    void evaluate() const {}
};

struct ordinary_two_input_sink {
    bg::In<bg::TS<float>> first;
    bg::In<bg::TS<float>> second;

    void evaluate() const {}
};

struct controlled_sink_definition {
    bg::In<bg::TS<float>, bg::dynamic_activity> input;
    void evaluate() const { input.make_passive(); }
};

class scale_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const bg::GenericTimeSeriesValueType input_type{bg::float_type(), bg::TimeSeriesGuarantee::ordinary};
        [[maybe_unused]] static const auto &input_type_registration = bg::TypeRegistry::register_static(input_type);
        static const bg::Signature value{
            bg::util::symbol{"scale"},
            {{bg::util::symbol{"input"}, &input_type}, {bg::util::symbol{"factor"}, &bg::float_type()}},
            bg::TS<float>::type()};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

  private:
    static bool evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) { return true; }
};

} // namespace

TEST_CASE("compiled nodes keep hot data in node-local sections", "[compiled_graph][feature:graph-compilation]") {
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    wiring.finalize();
    bg::CompiledGraph graph{wiring};

    REQUIRE(graph.node_count() == 2);
    const bg::CompiledNode &source_node = graph.node(0);
    const bg::CompiledNode &scale_node = graph.node(1);

    REQUIRE(source_node.subscribers().size() == 1);
    CHECK(source_node.subscribers()[0].node_index() == 1);
    CHECK(source_node.subscribers()[0].active());
    CHECK(source_node.subscribers()[0].required());
    CHECK_FALSE(source_node.subscribers()[0].active_optional());

    const auto parameters = scale_node.parameters();
    REQUIRE(parameters.size() == 2);
    CHECK(parameters[0].mode() == bg::ParameterBindingMode::time_series);
    CHECK(parameters[0].last_updated_offset() == source_node.output_last_updated_offset());
    CHECK(parameters[0].value_offset() == source_node.output_value_offset());
    CHECK(parameters[1].mode() == bg::ParameterBindingMode::scalar);
    CHECK(*static_cast<const float *>(parameters[1].scalar_data()) == 2.0F);

    const auto *node_address = reinterpret_cast<const std::byte *>(&scale_node);
    const auto *parameter_address = reinterpret_cast<const std::byte *>(parameters.data());
    const auto *scalar_address = static_cast<const std::byte *>(parameters[1].scalar_data());

    CHECK(parameter_address == node_address + sizeof(bg::CompiledNode));
    CHECK(scalar_address == parameter_address + parameters.size() * sizeof(bg::ParameterBinding));
    // Fixed storage includes graph metadata, node records, and the initial runtime storage image.
    CHECK(graph.fixed_storage_size() == 244);
}

TEST_CASE("all_valid metadata follows an empty runtime-type array at pointer alignment",
          "[compiled_graph][feature:validity][validity:all-valid][layout]") {
    struct source_definition {
        bg::Out<bg::TS<bg::i64>> output;

        void start() const { output.set(1); }
    };
    struct stateful_gate_definition {
        bg::State<bg::i64> evaluations;
        bg::In<bg::TS<bg::i64>, bg::all_valid> input;
        bg::Out<bg::TS<bg::i64>> output;

        void evaluate() const { output.set(++*evaluations); }
    };

    const auto source = bg::node<source_definition>();
    const auto gate = bg::node<stateful_gate_definition>();
    const auto gate_signature = gate.signature().resolve(std::array<const bg::Type *, 1>{&bg::TS<bg::i64>::type()});
    CHECK_FALSE(gate.uses_time_series_input_runtime_type(gate_signature, 0));
    CHECK_FALSE(gate.uses_output_runtime_type());

    const auto increment = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input + 1);
    });
    const auto double_value = bg::node([](bg::In<bg::TS<bg::i64>> input, bg::Out<bg::TS<bg::i64>> output) {
        output.set(*input * 2);
    });
    const auto combine = bg::node([](bg::In<bg::TS<bg::i64>> left, bg::In<bg::TS<bg::i64>> right,
                                     bg::Out<bg::TS<bg::i64>> output) { output.set(*left + *right); });

    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        const auto value = gate(source());
        static_cast<void>(combine(increment(value), double_value(value)));
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    const bg::CompiledNode *stateful_gate{};
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
    CHECK(descriptors[0].declared_type == &bg::TS<bg::i64>::type());
}

TEST_CASE("compiled node input producer indices use compiled node order",
          "[feature:graph-compilation][series:value][shape:chain]") {
    const auto source = bg::node<source_definition>();
    const auto sink = bg::node<input_node_index_sink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        const auto input = source();
        sink(input, input);
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    CHECK(graph.node(1).input_node_index(0) == 0);
    CHECK(graph.node(1).input_node_index(1) == 0);
    CHECK_FALSE(graph.node(0).input_node_index(0).has_value());

    const auto ordinary_sink = bg::node<ordinary_two_input_sink>();
    auto ordinary_wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{ordinary_wiring};
        const auto input = source();
        ordinary_sink(input, input);
    }
    ordinary_wiring.finalize();
    const auto ordinary_graph = bg::CompiledGraph{ordinary_wiring};
    CHECK(graph.fixed_storage_size() == ordinary_graph.fixed_storage_size() + 2 * sizeof(std::uint32_t));
}

TEST_CASE("compiled nodes retain all_valid metadata with input producer indices",
          "[feature:graph-compilation][feature:validity][series:value][scalar:i64][shape:chain]") {
    const auto source = bg::node<integer_source_definition>();
    const auto sink = bg::node<all_valid_input_node_index_sink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        sink(source());
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    const auto &compiled_sink = graph.node(1);
    CHECK(compiled_sink.input_node_index(0) == 0);
    CHECK(compiled_sink.has_all_valid_input_metadata());

    const auto descriptors = compiled_sink.all_valid_input_descriptors();
    REQUIRE(descriptors.size() == 1);
    CHECK(descriptors[0].parameter_index == 0);
    CHECK(descriptors[0].declared_type == &bg::TS<bg::i64>::type());
}

TEST_CASE("compiled node input producer indices omit lowered inputs and remap direct inputs",
          "[feature:graph-compilation][feature:constant][series:value][shape:chain][guarantee:const]") {
    using namespace bg::operators;

    const auto source = bg::node<source_definition>();
    const auto sink = bg::node<input_node_index_sink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        const auto constant = const_(1.0F).as<bg::TS<float, bg::TimeSeriesGuarantee::const_>>();
        sink(constant, source());
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    REQUIRE(graph.node_count() == 2);
    const auto &compiled_sink = graph.node(1);
    CHECK_FALSE(compiled_sink.input_node_index(0).has_value());
    CHECK(compiled_sink.input_node_index(1) == 0);
}

TEST_CASE("compiled nodes store one activity record for each controlled input",
          "[feature:input-activity][feature:graph-compilation][series:value][definition:aggregate]"
          "[shape:chain][activity:active]") {
    const auto source = bg::node<source_definition>();
    const auto sink = bg::node<controlled_sink_definition>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        sink(source());
    }

    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    CHECK(compiled.node(0).subscribers().empty());
    CHECK(compiled.node(0).output_notification() != nullptr);
    CHECK(compiled.node(1).controlled_inputs().size() == 1);
    CHECK(sizeof(bg::Subscriber) == sizeof(std::uint32_t));
    CHECK(sizeof(bg::ControlledInput) == sizeof(std::uint32_t));
}

namespace {

struct reference_subscription_source {
    bg::Out<bg::TS<bg::i64>> output;
    void evaluate() const {}
};

struct reference_subscription_pass {
    bg::In<bg::REF<bg::TS<bg::i64>>> input;
    bg::Out<bg::REF<bg::TS<bg::i64>>> output;
    void evaluate() const { output.copy(input); }
};

struct two_reference_input_sink {
    bg::In<bg::TS<bg::i64>> left;
    bg::In<bg::TS<bg::i64>> right;
    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled reference notifications identify each receiving input slot",
          "[compiled_graph][subscriptions][feature:graph-compilation]") {
    const auto source = bg::node<reference_subscription_source>();
    const auto pass = bg::node<reference_subscription_pass>();
    const auto sink = bg::node<two_reference_input_sink>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto reference = pass(source());
        sink(reference, reference);
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    const auto &reference_node = graph.node(2);
    const auto *notification = reference_node.output_notification();
    REQUIRE(notification != nullptr);
    const auto bytes = notification->records();
    const auto reference_subscribers = std::span{
        reinterpret_cast<const bg::details::ReferenceInputSubscriber *>(bytes.data()),
        bytes.size() / sizeof(bg::details::ReferenceInputSubscriber),
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
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    wiring.finalize();
    bg::CompiledGraph first{wiring};
    bg::CompiledGraph second{wiring};

    const void *first_scalar = first.node(1).parameters()[1].scalar_data();
    const void *second_scalar = second.node(1).parameters()[1].scalar_data();

    CHECK(first_scalar != second_scalar);
    CHECK(*static_cast<const float *>(first_scalar) == 2.0F);
    CHECK(*static_cast<const float *>(second_scalar) == 2.0F);
}

namespace {

struct passive_copy_definition {
    bg::In<bg::TS<float>, bg::passive, bg::optional> input;
    bg::Out<bg::TS<float>> output;

    void evaluate() const { output.set(*input); }
};

} // namespace

TEST_CASE("compiled parameters retain validity without a passive subscription",
          "[compiled_graph][feature:graph-compilation]") {
    const auto source = bg::node<source_definition>();
    const auto passive_copy = bg::node<passive_copy_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)passive_copy(source());
    }

    wiring.finalize();
    bg::CompiledGraph graph{wiring};

    CHECK(graph.node(0).subscribers().empty());
    CHECK(graph.node(1).parameters()[0].validity() == bg::optional);
}

TEST_CASE("a compiled graph owns its scalar copies", "[compiled_graph][feature:graph-compilation]") {
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    std::unique_ptr<bg::CompiledGraph> graph;

    {
        bg::GraphWiring wiring;
        bg::WiringContext context{wiring};
        (void)scale(source(), 2.0F);
        wiring.finalize();
        graph = std::make_unique<bg::CompiledGraph>(wiring);
    }

    const bg::ParameterBinding &factor = graph->node(1).parameters()[1];
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
    const auto source = bg::node<source_definition>();
    const auto scale = scale_template{};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)scale(source(), 2.0F);
    }

    counting_memory_resource memory;

    {
        wiring.finalize();
        bg::CompiledGraph graph{wiring, bg::allocator{memory}};

        CHECK(graph.fixed_storage_size() > 0);
        CHECK(memory.allocation_count() == 1);
    }

    CHECK(memory.allocation_count() == 0);
}

namespace {

struct evaluation_time_source_definition {
    bg::Out<bg::TS<bg::EvaluationTime>> output;

    void evaluate() const {}
};

} // namespace

TEST_CASE("a compiled graph stores an evaluation time output", "[compiled_graph][feature:graph-compilation]") {
    const auto evaluation_time_source = bg::node<evaluation_time_source_definition>();
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        (void)evaluation_time_source();
    }

    wiring.finalize();
    const bg::CompiledGraph graph{wiring};

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

    bg::Scalar<bg::Opaque<private_data *>> pointer;
    bg::Scalar<bg::Opaque<private_data>> object;

    void evaluate() const {
        observed = *pointer;
        observed_value = object->value;
    }
};

struct unsupported_opaque_output {
    bg::Out<bg::TS<bg::Opaque<private_data *>>> output;

    void evaluate() const {}
};

static_assert(bg::AggregateNodeClass<opaque_sink_definition>);
static_assert(!bg::AggregateNodeClass<unsupported_opaque_output>);

} // namespace

TEST_CASE("a compiled graph owns an opaque scalar copy", "[compiled_graph][feature:graph-compilation]") {
    const auto opaque_sink = bg::node<opaque_sink_definition>();
    auto value = private_data{42};
    const auto pointer = bg::ScalarValue<bg::Opaque<private_data *>>{&value};
    const auto object = bg::ScalarValue<bg::Opaque<private_data>>{value};
    bg::GraphWiring wiring;

    {
        bg::WiringContext context{wiring};
        opaque_sink(pointer, object);
    }

    wiring.finalize();
    const bg::CompiledGraph graph{wiring};
    const auto *stored_pointer = static_cast<private_data *const *>(graph.node(0).parameters()[0].scalar_data());
    const auto *stored_object = static_cast<const private_data *>(graph.node(0).parameters()[1].scalar_data());
    auto runtime_global_state = bg::GlobalState{};
    bg::RuntimeGraph runtime{graph, runtime_global_state};

    opaque_sink_definition::observed = nullptr;
    opaque_sink_definition::observed_value = 0;
    runtime.start(bg::MIN_DT + std::chrono::microseconds{1});
    runtime.schedule(0);
    runtime.evaluate(bg::MIN_DT + std::chrono::microseconds{1});

    CHECK(*stored_pointer == &value);
    CHECK(stored_object->value == 42);
    CHECK(opaque_sink_definition::observed == &value);
    CHECK(opaque_sink_definition::observed_value == 42);
}
