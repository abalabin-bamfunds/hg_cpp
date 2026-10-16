#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/queue.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace {

class LocalRuntimeType final : public bg::TimeSeriesValueRuntimeType {
  public:
    LocalRuntimeType()
        : TimeSeriesValueRuntimeType{bg::TS<bg::i64>::type(), bg::cpp_scalar_runtime_type<bg::i64>(),
                                     sizeof(bg::i64), alignof(bg::i64), false, false, true} {}

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::i64 *>(destination), bg::i64{});
    }
    void destroy_value(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<bg::i64 *>(value));
    }
};

struct QueueNodeDefinition {
    static constexpr bool uses_queue = true;

    QueueNodeDefinition(
        const bg::TimeSeriesRuntimeType &runtime_type = bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>())
        : runtime_type_{&runtime_type}, signature_{bg::util::symbol{"queue_node"}, {}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &) const noexcept {
        return *runtime_type_;
    }

    static void start(bg::NodeInvocation<>) {}
    static void evaluate(bg::NodeInvocation<>) {}
    static void stop(bg::NodeInvocation<>) noexcept {
        ++stop_count;
        if (stop_probe != nullptr) {
            stop_write_accepted = stop_probe->write(::literal::TimeSeriesLiteral(bg::i64{100}));
        }
    }

    inline static bg::Queue *stop_probe{};
    inline static bool stop_write_accepted{};
    inline static std::size_t stop_count{};

  private:
    const bg::TimeSeriesRuntimeType *runtime_type_;
    bg::Signature signature_;
};

struct FailingQueueNodeDefinition {
    static constexpr bool uses_queue = true;
    static constexpr bool schedule_on_start = true;

    FailingQueueNodeDefinition()
        : signature_{bg::util::symbol{"failing_queue_node"}, {}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &) const noexcept {
        return bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    }

    static void start(bg::NodeInvocation<> invocation) {
        queue = invocation.context().runtime_graph().make_queue(0);
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{1}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
        if (fail_start) {
            throw std::runtime_error{"start failure"};
        }
    }

    static void evaluate(bg::NodeInvocation<>) {
        if (fail_evaluation) {
            throw std::runtime_error{"evaluation failure"};
        }
    }

    static void stop(bg::NodeInvocation<>) noexcept {
        stop_write_accepted = queue.write(::literal::TimeSeriesLiteral(bg::i64{2}));
    }

    inline static bg::Queue queue;
    inline static bool fail_start{};
    inline static bool fail_evaluation{};
    inline static bool stop_write_accepted{};

  private:
    bg::Signature signature_;
};

struct QueuePayload {
    bg::i64 number{};
};

struct QueuePayloadBufferHandlers {
    static bool register_operations() {
        static const auto registered = [] {
            auto semantic = bg::ScalarBufferOperations::for_read<QueuePayload>(
                [](bg::util::buffer_reader &reader, auto consume) {
                    const auto number = reader.read_raw<bg::i64>();
                    reader.skip<std::shared_ptr<int>>();
                    const auto value = QueuePayload{number};
                    consume(bg::Scalar<QueuePayload>::from(value));
                });
            if (!semantic.set_discard([](bg::util::buffer_reader &reader) {
                    (void)reader.read_raw<bg::i64>();
                    reader.skip<std::shared_ptr<int>>();
                })) {
                return false;
            }

            auto runtime = bg::ScalarBufferOperations{};
            if (!runtime.set_write<QueuePayload>([](bg::Scalar<QueuePayload> value,
                                                    bg::ScalarBufferWriteContext &writer, bg::allocator &) {
                    const auto number = value.value().number;
                    writer.write_bytes(std::as_bytes(std::span{&number, std::size_t{1}}));
                    writer.write(writing_payload);
                }) ||
                !runtime.set_project<QueuePayload>([](bg::Scalar<QueuePayload> value,
                                                      bg::ScalarBufferProjectionContext &projection) {
                    (void)value;
                    projection.write_bytes(sizeof(bg::i64));
                    projection.emplace<std::shared_ptr<int>>();
                })) {
                return false;
            }

            auto &dispatch = bg::ScalarDispatch<bg::ScalarBufferOperations>::instance();
            return dispatch.add(bg::cpp_scalar_type<QueuePayload>(), std::move(semantic)) &&
                   dispatch.add(bg::cpp_scalar_runtime_type<QueuePayload>(), std::move(runtime));
        }();
        return registered;
    }

    inline static std::shared_ptr<int> writing_payload;
};

struct PayloadQueueNodeDefinition {
    static constexpr bool uses_queue = true;

    PayloadQueueNodeDefinition()
        : signature_{bg::util::symbol{"payload_queue_node"}, {}, bg::TS<QueuePayload>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &) const noexcept {
        return bg::cpp_time_series_runtime_type<bg::TS<QueuePayload>>();
    }

    static void start(bg::NodeInvocation<>) {}
    static void evaluate(bg::NodeInvocation<>) {}
    static void stop(bg::NodeInvocation<>) noexcept {
        payload_alive_during_stop = !payload_probe.expired();
    }

    inline static std::weak_ptr<int> payload_probe;
    inline static bool payload_alive_during_stop{};

  private:
    bg::Signature signature_;
};

struct PayloadFailingQueueNodeDefinition {
    static constexpr bool uses_queue = true;
    static constexpr bool schedule_on_start = true;

    PayloadFailingQueueNodeDefinition()
        : signature_{bg::util::symbol{"payload_failing_queue_node"}, {}, bg::TS<QueuePayload>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &) const noexcept {
        return bg::cpp_time_series_runtime_type<bg::TS<QueuePayload>>();
    }

    static void start(bg::NodeInvocation<> invocation) {
        queue = invocation.context().runtime_graph().make_queue(0);
        auto payload = std::make_shared<int>(31);
        payload_probe = payload;
        QueuePayloadBufferHandlers::writing_payload = payload;
        auto value = bg::ScalarValue<QueuePayload>{QueuePayload{31}};
        const auto accepted = queue.write(::literal::TimeSeriesLiteral(value));
        QueuePayloadBufferHandlers::writing_payload.reset();
        if (!accepted) {
            throw std::logic_error{"queue write was rejected during start"};
        }
        if (fail_start) {
            throw std::runtime_error{"payload queue start failure"};
        }
    }

    static void evaluate(bg::NodeInvocation<>) {
        if (fail_evaluation) {
            throw std::runtime_error{"payload queue evaluation failure"};
        }
    }

    static void stop(bg::NodeInvocation<>) noexcept { payload_alive_during_stop = !payload_probe.expired(); }

    inline static bg::Queue queue;
    inline static std::weak_ptr<int> payload_probe;
    inline static bool payload_alive_during_stop{};
    inline static bool fail_start{};
    inline static bool fail_evaluation{};

  private:
    bg::Signature signature_;
};

template <bool Timed, bool Cycle, bool Queue = true>
struct QueueLayoutNodeDefinition : QueueNodeDefinition {
    static constexpr bool uses_queue = Queue;
    static constexpr bool uses_timed_scheduling = Timed;
    static constexpr bool uses_cycle_callbacks = Cycle;

    static void start(bg::NodeInvocation<> invocation) {
        if constexpr (Timed) {
            invocation.context().schedule_at(invocation.context().evaluation_time() + std::chrono::microseconds{2});
        }
        if constexpr (Cycle) {
            invocation.context().add_cycle_callback(nullptr, &run_cycle_callback);
        }
    }

    static void run_cycle_callback(void *, bg::EvaluationContext &) noexcept { ++cycle_callback_count; }

    inline static std::size_t cycle_callback_count{};
};

[[nodiscard]] bg::EvaluationTime time(bg::i64 microseconds) {
    return bg::MIN_DT + std::chrono::microseconds{microseconds};
}

} // namespace

TEST_CASE("an outer runtime graph shares and closes queues before stop callbacks",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][lifecycle]") {
    auto first_node = bg::node(QueueNodeDefinition{});
    auto second_node = bg::node(QueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    std::optional<bg::WiringPoint> first_output;
    std::optional<bg::WiringPoint> second_output;
    {
        auto context = bg::WiringContext{wiring};
        first_output = first_node();
        second_output = second_node();
    }
    wiring.finalize();

    const auto first_target = wiring.output_node_index(*first_output);
    const auto second_target = wiring.output_node_index(*second_output);
    const auto compiled = bg::CompiledGraph{wiring};
    REQUIRE(compiled.has_queue());
    CHECK_FALSE(compiled.supports_borrowed_evaluation());

    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    CHECK_THROWS_AS(runtime.make_queue(first_target), std::logic_error);
    runtime.start(time(1));

    auto first = runtime.make_queue(first_target);
    auto second = runtime.make_queue(second_target);
    REQUIRE(first.write(::literal::TimeSeriesLiteral(bg::i64{11})));
    REQUIRE(second.write(::literal::TimeSeriesLiteral(bg::i64{22})));
    QueueNodeDefinition::stop_probe = &first;
    QueueNodeDefinition::stop_write_accepted = true;
    QueueNodeDefinition::stop_count = 0;

    runtime.stop();

    CHECK(QueueNodeDefinition::stop_count == 2);
    CHECK_FALSE(QueueNodeDefinition::stop_write_accepted);
    CHECK_FALSE(first.write(::literal::TimeSeriesLiteral(bg::i64{33})));
    CHECK_FALSE(second.write(::literal::TimeSeriesLiteral(bg::i64{44})));
    QueueNodeDefinition::stop_probe = nullptr;
}

TEST_CASE("a stopped runtime graph creates closed queue handles",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][lifecycle]") {
    auto node = bg::node(QueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};

    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto queue = runtime.make_queue(0);
        runtime.stop();

        auto closed = runtime.make_queue(0);
        CHECK_FALSE(closed.write(::literal::TimeSeriesLiteral(bg::i64{1})));
        CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{2})));
    }

    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(2));
        runtime.stop();

        auto closed = runtime.make_queue(0);
        CHECK_FALSE(closed.write(::literal::TimeSeriesLiteral(bg::i64{3})));
    }
}

TEST_CASE("queue shutdown handles startup and evaluation failures",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][failure]") {
    auto node = bg::node(FailingQueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};

    FailingQueueNodeDefinition::fail_start = true;
    FailingQueueNodeDefinition::fail_evaluation = false;
    FailingQueueNodeDefinition::stop_write_accepted = true;
    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        CHECK_THROWS_AS(runtime.start(time(1)), std::runtime_error);
        CHECK_FALSE(FailingQueueNodeDefinition::queue.write(::literal::TimeSeriesLiteral(bg::i64{3})));
        CHECK(FailingQueueNodeDefinition::stop_write_accepted);
    }

    FailingQueueNodeDefinition::fail_start = false;
    FailingQueueNodeDefinition::fail_evaluation = true;
    FailingQueueNodeDefinition::stop_write_accepted = true;
    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(2));
        CHECK_THROWS_AS(runtime.evaluate(time(3)), std::runtime_error);
        CHECK_FALSE(FailingQueueNodeDefinition::stop_write_accepted);
        CHECK_FALSE(FailingQueueNodeDefinition::queue.write(::literal::TimeSeriesLiteral(bg::i64{4})));
    }
    FailingQueueNodeDefinition::fail_start = false;
    FailingQueueNodeDefinition::fail_evaluation = false;
}

TEST_CASE("finalization closes and discards queues without invoking stop functions",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][finalization]") {
    auto node = bg::node(QueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    QueueNodeDefinition::stop_count = 0;
    runtime.start(time(1));
    auto queue = runtime.make_queue(0);
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{55})));

    runtime.finalize(global_state);

    CHECK(QueueNodeDefinition::stop_count == 0);
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{56})));
    CHECK_THROWS_AS(runtime.make_queue(0), std::logic_error);
}

TEST_CASE("queue discard destroys unread typed payloads after stop callbacks and finalization",
          "[evaluation][feature:time-series-storage][series:value][scalar:compound][queue][lifecycle]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    auto integer_node = bg::node(QueueNodeDefinition{});
    auto payload_node = bg::node(PayloadQueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    std::optional<bg::WiringPoint> integer_output;
    std::optional<bg::WiringPoint> payload_output;
    {
        auto context = bg::WiringContext{wiring};
        integer_output = integer_node();
        payload_output = payload_node();
    }
    wiring.finalize();
    const auto integer_target = wiring.output_node_index(*integer_output);
    const auto payload_target = wiring.output_node_index(*payload_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};

    SECTION("stop callbacks run before unread payload destruction") {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto integer_queue = runtime.make_queue(integer_target);
        auto payload_queue = runtime.make_queue(payload_target);
        REQUIRE(integer_queue.write(::literal::TimeSeriesLiteral(bg::i64{7})));
        auto payload = std::make_shared<int>(17);
        PayloadQueueNodeDefinition::payload_probe = payload;
        QueuePayloadBufferHandlers::writing_payload = payload;
        {
            auto value = bg::ScalarValue<QueuePayload>{QueuePayload{17}};
            REQUIRE(payload_queue.write(::literal::TimeSeriesLiteral(value)));
        }
        QueuePayloadBufferHandlers::writing_payload.reset();
        payload.reset();
        CHECK_FALSE(PayloadQueueNodeDefinition::payload_probe.expired());
        PayloadQueueNodeDefinition::payload_alive_during_stop = false;

        runtime.stop();

        CHECK(PayloadQueueNodeDefinition::payload_alive_during_stop);
        CHECK(PayloadQueueNodeDefinition::payload_probe.expired());
        CHECK_FALSE(integer_queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
        CHECK_FALSE(payload_queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    }

    SECTION("finalization discards unread payloads without stop callbacks") {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto integer_queue = runtime.make_queue(integer_target);
        auto payload_queue = runtime.make_queue(payload_target);
        REQUIRE(integer_queue.write(::literal::TimeSeriesLiteral(bg::i64{8})));
        auto payload = std::make_shared<int>(23);
        PayloadQueueNodeDefinition::payload_probe = payload;
        QueuePayloadBufferHandlers::writing_payload = payload;
        {
            auto value = bg::ScalarValue<QueuePayload>{QueuePayload{23}};
            REQUIRE(payload_queue.write(::literal::TimeSeriesLiteral(value)));
        }
        QueuePayloadBufferHandlers::writing_payload.reset();
        payload.reset();
        PayloadQueueNodeDefinition::payload_alive_during_stop = false;

        runtime.finalize(global_state);

        CHECK(PayloadQueueNodeDefinition::payload_probe.expired());
        CHECK_FALSE(PayloadQueueNodeDefinition::payload_alive_during_stop);
        CHECK_FALSE(integer_queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
        CHECK_FALSE(payload_queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    }
}

TEST_CASE("queue shutdown destroys typed unread payloads after startup and evaluation failures",
          "[evaluation][feature:time-series-storage][series:value][scalar:compound][queue][failure]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    auto node = bg::node(PayloadFailingQueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};

    PayloadFailingQueueNodeDefinition::fail_start = true;
    PayloadFailingQueueNodeDefinition::fail_evaluation = false;
    PayloadFailingQueueNodeDefinition::payload_alive_during_stop = false;
    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        CHECK_THROWS_AS(runtime.start(time(1)), std::runtime_error);
        CHECK_FALSE(PayloadFailingQueueNodeDefinition::payload_alive_during_stop);
        CHECK(PayloadFailingQueueNodeDefinition::payload_probe.expired());
        CHECK_FALSE(PayloadFailingQueueNodeDefinition::queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    }

    PayloadFailingQueueNodeDefinition::fail_start = false;
    PayloadFailingQueueNodeDefinition::fail_evaluation = true;
    PayloadFailingQueueNodeDefinition::payload_alive_during_stop = false;
    {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(2));
        CHECK_THROWS_AS(runtime.evaluate(time(3)), std::runtime_error);
        CHECK(PayloadFailingQueueNodeDefinition::payload_alive_during_stop);
        CHECK(PayloadFailingQueueNodeDefinition::payload_probe.expired());
        CHECK_FALSE(PayloadFailingQueueNodeDefinition::queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    }
    PayloadFailingQueueNodeDefinition::fail_evaluation = false;
}

TEST_CASE("runtime graph queues have independent lifetimes",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][lifecycle]") {
    auto node = bg::node(QueueNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto first_runtime = bg::RuntimeGraph{compiled, global_state};
    auto second_runtime = bg::RuntimeGraph{compiled, global_state};
    first_runtime.start(time(1));
    second_runtime.start(time(1));
    auto first_queue = first_runtime.make_queue(0);
    auto second_queue = second_runtime.make_queue(0);

    first_runtime.stop();

    CHECK_FALSE(first_queue.write(::literal::TimeSeriesLiteral(bg::i64{1})));
    CHECK(second_queue.write(::literal::TimeSeriesLiteral(bg::i64{2})));

    second_runtime.stop();
    CHECK_FALSE(second_queue.write(::literal::TimeSeriesLiteral(bg::i64{3})));
}

TEST_CASE("inner runtime graphs cannot create graph queues",
          "[evaluation][nested][feature:nested-graph][series:value][queue][lifecycle]") {
    auto node = bg::node(QueueNodeDefinition{});
    auto outer_wiring = bg::GraphWiring{};
    auto inner_wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{outer_wiring};
        (void)node();
    }
    {
        auto context = bg::WiringContext{inner_wiring};
        (void)node();
    }
    outer_wiring.finalize();
    inner_wiring.finalize();
    const auto outer_compiled = bg::CompiledGraph{outer_wiring};
    const auto inner_compiled = bg::CompiledGraph{inner_wiring};
    auto global_state = bg::GlobalState{};
    auto outer = bg::RuntimeGraph{outer_compiled, global_state};
    auto evaluator = bg::RuntimeGraphEvaluator{};
    evaluator.bind_owner(outer, 0);
    auto inner = bg::RuntimeGraph{inner_compiled, outer.allocation(), evaluator, 0};

    outer.start(time(1));
    inner.start(time(1), global_state);
    CHECK_THROWS_AS(inner.make_queue(0), std::logic_error);
    inner.stop(global_state);
    inner.finalize(global_state);
    outer.stop();
}

TEST_CASE("queue storage follows timed scheduling and cycle callback pointers",
          "[evaluation][feature:time-series-storage][queue][layout]") {
    const auto verify_layout = []<typename Definition, typename WithoutQueue>(std::size_t expected_offset) {
        auto node = bg::node(Definition{});
        auto node_without_queue = bg::node(WithoutQueue{});
        auto wiring = bg::GraphWiring{};
        auto wiring_without_queue = bg::GraphWiring{};
        {
            auto context = bg::WiringContext{wiring};
            (void)node();
        }
        {
            auto context = bg::WiringContext{wiring_without_queue};
            (void)node_without_queue();
        }
        wiring.finalize();
        wiring_without_queue.finalize();
        const auto compiled = bg::CompiledGraph{wiring};
        const auto compiled_without_queue = bg::CompiledGraph{wiring_without_queue};
        CHECK(compiled.has_queue());
        CHECK(compiled.queue_offset() == expected_offset);
        CHECK_FALSE(compiled_without_queue.has_queue());
        CHECK(compiled.runtime_storage_size() == compiled_without_queue.runtime_storage_size() + sizeof(void *));

        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        Definition::cycle_callback_count = 0;
        runtime.start(time(1));
        auto queue = runtime.make_queue(0);
        REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{42})));
        if constexpr (Definition::uses_timed_scheduling) {
            CHECK(runtime.next_scheduled_time() == time(3));
        }
        runtime.evaluate(time(1));
        if constexpr (Definition::uses_cycle_callbacks) {
            CHECK(Definition::cycle_callback_count > 0);
        }
        runtime.stop();
        CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    };

    verify_layout.template operator()<QueueLayoutNodeDefinition<false, false>,
                                      QueueLayoutNodeDefinition<false, false, false>>(0);
    verify_layout.template operator()<QueueLayoutNodeDefinition<true, false>,
                                      QueueLayoutNodeDefinition<true, false, false>>(sizeof(void *));
    verify_layout.template operator()<QueueLayoutNodeDefinition<false, true>,
                                      QueueLayoutNodeDefinition<false, true, false>>(sizeof(void *));
    verify_layout.template operator()<QueueLayoutNodeDefinition<true, true>,
                                      QueueLayoutNodeDefinition<true, true, false>>(2 * sizeof(void *));
}

TEST_CASE("a retained graph queue is closed before its output runtime type expires",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][lifetime]") {
    auto retained = bg::Queue{};
    {
        auto runtime_type = LocalRuntimeType{};
        auto node = bg::node(QueueNodeDefinition{runtime_type});
        auto wiring = bg::GraphWiring{};
        std::optional<bg::WiringPoint> output;
        {
            auto context = bg::WiringContext{wiring};
            output = node();
        }
        wiring.finalize();
        const auto target = wiring.output_node_index(*output);
        const auto compiled = bg::CompiledGraph{wiring};
        auto global_state = bg::GlobalState{};
        {
            auto runtime = bg::RuntimeGraph{compiled, global_state};
            runtime.start(time(1));
            retained = runtime.make_queue(target);
            REQUIRE(retained.write(::literal::TimeSeriesLiteral(bg::i64{77})));
        }
    }

    CHECK_FALSE(retained.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
}
