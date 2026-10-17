#include <bg/graph/compiled_graph.hpp>
#include <bg/graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/runtime_graph_evaluator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/queue.hpp>
#include <bg/simulation_evaluation_engine.hpp>
#include <bg/scalar/buffer_operations.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/util/buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

template <template <typename, auto...> typename Field> struct BatchQueueBundle {
    Field<bg::TS<bg::List<bg::i64>>> first;
    Field<bg::TS<bg::List<bg::i64>>> second;
};

struct QueuePayloadBufferHandlers {
    static bool register_operations() {
        static const auto registered = [] {
            auto semantic = bg::ScalarBufferOperations::for_read<QueuePayload>(
                [](bg::util::buffer_reader &reader, auto consume) {
                    if (++read_calls == fail_on_read) {
                        throw std::runtime_error{"injected queue payload read failure"};
                    }
                    const auto number = reader.read_raw<bg::i64>();
                    reader.consume<std::shared_ptr<int>>();
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
                    if (fail_after_write) {
                        fail_after_write = false;
                        throw std::runtime_error{"injected queue payload write failure"};
                    }
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
    inline static std::size_t read_calls{};
    inline static std::size_t fail_on_read{};
    inline static bool fail_after_write{};
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
        const auto retained = queue.write(::literal::TimeSeriesLiteral(value));
        QueuePayloadBufferHandlers::writing_payload.reset();
        if (!accepted || !retained) {
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

TEST_CASE("batch queue shutdown and failed writes discard owning item payloads",
          "[evaluation][feature:time-series-storage][series:value][scalar:compound][scalar:list][queue][batch]"
          "[lifecycle][failure]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    const auto queue_node = bg::queue<bg::TS<bg::List<QueuePayload>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};

    SECTION("shutdown discards the semantic batch item") {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto queue = runtime.make_queue(target);
        auto payload = std::make_shared<int>(41);
        auto probe = std::weak_ptr<int>{payload};
        QueuePayloadBufferHandlers::writing_payload = payload;
        auto value = bg::ScalarValue<QueuePayload>{QueuePayload{41}};
        REQUIRE(queue.write(::literal::TimeSeriesLiteral(value)));
        QueuePayloadBufferHandlers::writing_payload.reset();
        payload.reset();
        CHECK_FALSE(probe.expired());

        runtime.stop();

        CHECK(probe.expired());
    }

    SECTION("writer rollback discards the semantic batch item") {
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        auto queue = runtime.make_queue(target);
        auto payload = std::make_shared<int>(42);
        auto probe = std::weak_ptr<int>{payload};
        QueuePayloadBufferHandlers::writing_payload = payload;
        auto value = bg::ScalarValue<QueuePayload>{QueuePayload{42}};
        QueuePayloadBufferHandlers::fail_after_write = true;
        CHECK_THROWS_AS(queue.write(::literal::TimeSeriesLiteral(value)), std::runtime_error);
        QueuePayloadBufferHandlers::writing_payload.reset();
        payload.reset();
        CHECK(probe.expired());
        runtime.stop();
    }
}

TEST_CASE("batch queue replay failure discards later values in the same nested frame",
          "[evaluation][feature:time-series-storage][series:fixed-tsl][scalar:compound][scalar:list][queue][batch]"
          "[failure]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    const auto queue_node = bg::queue<bg::TSL<bg::TS<bg::List<QueuePayload>>, 3>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    auto payload = std::make_shared<int>(43);
    auto probe = std::weak_ptr<int>{payload};
    QueuePayloadBufferHandlers::writing_payload = payload;
    QueuePayloadBufferHandlers::read_calls = 0;
    QueuePayloadBufferHandlers::fail_on_read = 5;
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{
        {0, QueuePayload{1}}, {1, QueuePayload{2}}, {2, QueuePayload{3}}}));
    QueuePayloadBufferHandlers::writing_payload.reset();
    payload.reset();
    CHECK_FALSE(probe.expired());

    CHECK_THROWS_AS(runtime.evaluate(time(2)), std::runtime_error);
    CHECK(QueuePayloadBufferHandlers::read_calls == 5);
    QueuePayloadBufferHandlers::fail_on_read = 0;
    CHECK(probe.expired());
    runtime.stop();
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
            CHECK(runtime.next_scheduled_time() == time(2));
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

TEST_CASE("pending queue messages schedule a minimum-tick simulation cycle",
          "[evaluation][simulation][queue][feature:simulation][feature:time-series-storage]"
          "[feature:timed-scheduling][series:value][scalar:i64]") {
    auto values = std::vector<std::pair<bg::i64, bg::EvaluationTime>>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{1})) ||
            !queue.write(::literal::TimeSeriesLiteral(bg::i64{2}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });
    const auto observer = bg::node([&](bg::In<bg::TS<bg::i64>> input) {
        values.emplace_back(*input, input.last_updated());
    });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        observer(queue_node());
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}, {2, time(2)}});
}

struct QueueTimerCancellation {
    static constexpr bool uses_timed_scheduling = true;
    static constexpr bool schedule_on_start = true;

    void start(bg::EvaluationContext *context) const {
        handle = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const { context->cancel(handle); }

    inline static bg::ScheduleHandle handle;
};

TEST_CASE("cancelling a timer lets pending queue work use the minimum-tick cycle",
          "[evaluation][queue][feature:time-series-storage][feature:timed-scheduling]"
          "[series:value][scalar:i64][transition:removal]") {
    auto values = std::vector<std::pair<bg::i64, bg::EvaluationTime>>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{1})) ||
            !queue.write(::literal::TimeSeriesLiteral(bg::i64{2}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });
    const auto observer = bg::node([&](bg::In<bg::TS<bg::i64>> input) {
        values.emplace_back(*input, input.last_updated());
    });
    const auto timer = bg::node<QueueTimerCancellation>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        observer(queue_node());
        timer();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    QueueTimerCancellation::handle = {};
    runtime.start(time(1));
    runtime.evaluate(time(1));

    CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}});
    REQUIRE(runtime.next_scheduled_time() == time(2));

    runtime.evaluate(time(2));

    CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}, {2, time(2)}});
    CHECK_FALSE(runtime.next_scheduled_time().has_value());
    runtime.stop();
}

struct QueueDueTimer {
    static constexpr bool uses_timed_scheduling = true;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const {}
};

TEST_CASE("due and overdue timers leave pending queue frames for the next minimum-tick cycle",
          "[evaluation][queue][feature:time-series-storage][feature:timed-scheduling]"
          "[series:value][scalar:i64]") {
    auto values = std::vector<std::pair<bg::i64, bg::EvaluationTime>>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{1})) ||
            !queue.write(::literal::TimeSeriesLiteral(bg::i64{2}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });
    const auto observer = bg::node([&](bg::In<bg::TS<bg::i64>> input) {
        values.emplace_back(*input, input.last_updated());
    });
    const auto timer = bg::node<QueueDueTimer>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        observer(queue_node());
        timer();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    for (const auto next_cycle : std::array{time(3), time(10)}) {
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        values.clear();
        runtime.start(time(1));
        runtime.evaluate(time(1));
        CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}});
        REQUIRE(runtime.next_scheduled_time() == time(2));

        runtime.evaluate(next_cycle);

        CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}});
        REQUIRE(runtime.next_scheduled_time() == next_cycle + std::chrono::microseconds{1});
        runtime.evaluate(next_cycle + std::chrono::microseconds{1});

        CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{
                            {1, time(1)}, {2, next_cycle + std::chrono::microseconds{1}}});
        runtime.stop();
    }
}

struct QueuePriorityInnerTimer {
    static constexpr bool uses_timed_scheduling = true;

    bg::State<int> step;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
    }

    void evaluate(bg::EvaluationContext *context) const {
        evaluation_times.push_back(context->evaluation_time());
        ++*step;
        output.set(*step);
        if (*step < 3) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    inline static std::vector<bg::EvaluationTime> evaluation_times;
};

TEST_CASE("nested minimum-tick timers keep queued frames unread until the chain completes",
          "[evaluation][simulation][nested][queue][feature:simulation][feature:time-series-storage]"
          "[feature:timed-scheduling][feature:nested-graph][series:value][scalar:i64]") {
    using namespace bg::literals;

    auto values = std::vector<std::pair<bg::i64, bg::EvaluationTime>>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue queue) {
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{1})) ||
            !queue.write(::literal::TimeSeriesLiteral(bg::i64{2}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    });
    const auto observer = bg::node([&](bg::In<bg::TS<bg::i64>> input) {
        values.emplace_back(*input, input.last_updated());
    });
    const auto timer = bg::node<QueuePriorityInnerTimer>();
    const auto inner = bg::graph("name"_n = "queue_priority_inner_timer", [&]() -> bg::wp<bg::TS<bg::i64>> {
        return timer();
    });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        observer(queue_node());
        (void)bg::nested(inner, bg::operators::const_(true));
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    QueuePriorityInnerTimer::evaluation_times.clear();

    bg::SimulationEvaluationEngine{}.run(runtime, time(1));

    CHECK(QueuePriorityInnerTimer::evaluation_times == std::vector{time(2), time(3), time(4)});
    CHECK(values == std::vector<std::pair<bg::i64, bg::EvaluationTime>>{{1, time(1)}, {2, time(5)}});
}

struct QueueTimedCleanupNodeDefinition {
    static constexpr bool uses_queue = true;
    static constexpr bool uses_timed_scheduling = true;

    QueueTimedCleanupNodeDefinition()
        : signature_{bg::util::symbol{"queue_timed_cleanup_node"}, {}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const noexcept { return signature_; }
    bool uses_output_runtime_type() const noexcept { return true; }
    const bg::TimeSeriesRuntimeType &output_runtime_type(const bg::WiringArguments &) const noexcept {
        return bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>();
    }

    static void start(bg::NodeInvocation<> invocation) {
        auto &context = invocation.context();
        queue = context.runtime_graph().make_queue(0);
        context.schedule_at(context.evaluation_time() + std::chrono::microseconds{2});
        if (!queue.write(::literal::TimeSeriesLiteral(bg::i64{42}))) {
            throw std::logic_error{"queue write was rejected during start"};
        }
    }

    static void evaluate(bg::NodeInvocation<>) {}

    static void stop(bg::NodeInvocation<> invocation) noexcept {
        next_scheduled_time_during_stop = invocation.context().runtime_graph().next_scheduled_time();
    }

    inline static bg::Queue queue;
    inline static std::optional<bg::EvaluationTime> next_scheduled_time_during_stop;

  private:
    bg::Signature signature_;
};

TEST_CASE("stopped and finalized graphs suppress queue readiness and preserve pending timer reporting",
          "[evaluation][queue][lifecycle][feature:time-series-storage][feature:timed-scheduling]"
          "[series:value][scalar:i64]") {
    const auto node = bg::node(QueueTimedCleanupNodeDefinition{});
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)node();
    }
    wiring.finalize();

    const auto compiled = bg::CompiledGraph{wiring};
    {
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        REQUIRE(runtime.next_scheduled_time() == time(2));

        runtime.stop();

        CHECK(QueueTimedCleanupNodeDefinition::next_scheduled_time_during_stop == time(3));
        CHECK_FALSE(runtime.next_scheduled_time().has_value());
    }

    {
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        runtime.start(time(1));
        REQUIRE(runtime.next_scheduled_time() == time(2));

        runtime.finalize(global_state);

        CHECK(runtime.next_scheduled_time() == time(3));
    }
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

TEST_CASE("outer evaluation drains standard and eliding queues and publishes each changed target once",
          "[evaluation][feature:time-series-storage][series:value][scalar:i64][queue][transition:update]") {
    auto standard_values = std::vector<bg::i64>{};
    auto elided_values = std::vector<bg::i64>{};
    const auto standard_queue = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto elided_queue = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::elide);
    const auto standard_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { standard_values.push_back(*input); });
    const auto elided_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { elided_values.push_back(*input); });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint standard_output;
    bg::WiringPoint elided_output;
    {
        const auto context = bg::WiringContext{wiring};
        standard_output = standard_queue();
        elided_output = elided_queue();
        standard_sink(standard_output);
        elided_sink(elided_output);
    }
    wiring.finalize();
    const auto standard_target = wiring.output_node_index(standard_output);
    const auto elided_target = wiring.output_node_index(elided_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    auto standard = runtime.make_queue(standard_target);
    auto elided = runtime.make_queue(elided_target);
    CHECK_FALSE(standard.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    CHECK_FALSE(elided.write(::literal::TimeSeriesLiteral(::literal::SKIP)));
    runtime.evaluate(time(2));
    CHECK(standard_values.empty());
    CHECK(elided_values.empty());

    REQUIRE(elided.write(::literal::TimeSeriesLiteral(bg::i64{20})));
    REQUIRE(elided.write(::literal::TimeSeriesLiteral(bg::i64{21})));
    REQUIRE(standard.write(::literal::TimeSeriesLiteral(bg::i64{10})));
    REQUIRE(standard.write(::literal::TimeSeriesLiteral(bg::i64{11})));
    REQUIRE(standard.write(::literal::TimeSeriesLiteral(bg::i64{12})));
    runtime.evaluate(time(3));

    CHECK(standard_values == std::vector<bg::i64>{10});
    CHECK(elided_values == std::vector<bg::i64>{21});
    CHECK(runtime.node(standard_target).output_last_updated().time() == time(3));
    CHECK(runtime.node(elided_target).output_last_updated().time() == time(3));

    runtime.evaluate(time(4));
    CHECK(standard_values == std::vector<bg::i64>{10, 11});
    CHECK(elided_values == std::vector<bg::i64>{21});
    runtime.evaluate(time(5));
    CHECK(standard_values == std::vector<bg::i64>{10, 11, 12});

    runtime.schedule(elided_target);
    runtime.evaluate(time(6));
    CHECK(standard_values == std::vector<bg::i64>{10, 11, 12});
    CHECK(elided_values == std::vector<bg::i64>{21});
    runtime.stop();
}

TEST_CASE("batch queues append copied handle values and encode nested scalar list items",
          "[evaluation][feature:time-series-storage][series:value][scalar:list][scalar:i64][queue][batch]") {
    auto batch_values = std::vector<std::vector<bg::i64>>{};
    auto nested_values = std::vector<std::vector<std::vector<bg::i64>>>{};
    const auto batch_queue = bg::queue<bg::TS<bg::List<bg::i64>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto nested_queue = bg::queue<bg::TS<bg::List<bg::List<bg::i64>>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto batch_sink = bg::node([&](bg::In<bg::TS<bg::List<bg::i64>>> input) {
        auto values = std::vector<bg::i64>{};
        const auto list = input.value_view();
        for (auto index = std::size_t{}; index < list.size(); ++index) {
            values.push_back(list[index].value());
        }
        batch_values.push_back(std::move(values));
    });
    const auto nested_sink = bg::node([&](bg::In<bg::TS<bg::List<bg::List<bg::i64>>>> input) {
        auto values = std::vector<std::vector<bg::i64>>{};
        const auto outer = input.value_view();
        for (auto index = std::size_t{}; index < outer.size(); ++index) {
            auto inner_values = std::vector<bg::i64>{};
            const auto inner = outer[index];
            for (auto item = std::size_t{}; item < inner.size(); ++item) {
                inner_values.push_back(inner[item].value());
            }
            values.push_back(std::move(inner_values));
        }
        nested_values.push_back(std::move(values));
    });

    auto wiring = bg::GraphWiring{};
    auto batch_output = bg::WiringPoint{};
    auto nested_output = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        batch_output = batch_queue();
        nested_output = nested_queue();
        batch_sink(batch_output);
        nested_sink(nested_output);
    }
    wiring.finalize();
    const auto batch_target = wiring.output_node_index(batch_output);
    const auto nested_target = wiring.output_node_index(nested_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    auto batch = runtime.make_queue(batch_target);
    auto batch_copy = batch;
    auto nested = runtime.make_queue(nested_target);
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{10})));
    REQUIRE(batch_copy.write(::literal::TimeSeriesLiteral(bg::i64{20})));
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{30})));
    REQUIRE(nested.write(::literal::TimeSeriesLiteral{1, 2}));
    REQUIRE(nested.write(::literal::TimeSeriesLiteral{3}));
    runtime.evaluate(time(2));

    CHECK(batch_values == std::vector<std::vector<bg::i64>>{{10, 20, 30}});
    CHECK(nested_values == std::vector<std::vector<std::vector<bg::i64>>>{{{1, 2}, {3}}});

    REQUIRE(batch_copy.write(::literal::TimeSeriesLiteral(bg::i64{40})));
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{50})));
    runtime.evaluate(time(3));
    CHECK(batch_values == std::vector<std::vector<bg::i64>>{{10, 20, 30}, {40, 50}});
    CHECK(nested_values == std::vector<std::vector<std::vector<bg::i64>>>{{{1, 2}, {3}}});
    runtime.stop();
}

TEST_CASE("batch queues accumulate scalar lists inside TSL, TSB, and TSD values",
          "[evaluation][feature:time-series-storage][series:fixed-tsl][series:tsb][series:tsd]"
          "[scalar:list][scalar:i64][queue][batch]") {
    auto list_values = std::vector<std::vector<std::vector<bg::i64>>>{};
    auto bundle_values = std::vector<std::vector<std::vector<bg::i64>>>{};
    auto dictionary_values = std::vector<std::vector<std::vector<bg::i64>>>{};
    const auto list_queue = bg::queue<bg::TSL<bg::TS<bg::List<bg::i64>>, 2>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto bundle_queue = bg::queue<bg::TSB<BatchQueueBundle>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto dictionary_queue = bg::queue<bg::TSD<bg::i64, bg::TS<bg::List<bg::i64>>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto list_sink = bg::node([&](bg::In<bg::TSL<bg::TS<bg::List<bg::i64>>, 2>> input) {
        auto values = std::vector<std::vector<bg::i64>>{};
        for (auto index = std::size_t{}; index < input.size(); ++index) {
            auto items = std::vector<bg::i64>{};
            const auto list = input[index].value_view();
            for (auto item = std::size_t{}; item < list.size(); ++item) {
                items.push_back(list[item].value());
            }
            values.push_back(std::move(items));
        }
        list_values.push_back(std::move(values));
    });
    const auto bundle_sink = bg::node([&](bg::In<bg::TSB<BatchQueueBundle>> input) {
        auto values = std::vector<std::vector<bg::i64>>{};
        for (const auto field : {input->first, input->second}) {
            auto items = std::vector<bg::i64>{};
            const auto list = field.value_view();
            for (auto item = std::size_t{}; item < list.size(); ++item) {
                items.push_back(list[item].value());
            }
            values.push_back(std::move(items));
        }
        bundle_values.push_back(std::move(values));
    });
    const auto dictionary_sink = bg::node([&](bg::In<bg::TSD<bg::i64, bg::TS<bg::List<bg::i64>>>> input) {
        auto values = std::vector<std::vector<bg::i64>>{};
        for (const auto key : {bg::i64{1}, bg::i64{2}}) {
            auto items = std::vector<bg::i64>{};
            const auto list = input[key].value_view();
            for (auto item = std::size_t{}; item < list.size(); ++item) {
                items.push_back(list[item].value());
            }
            values.push_back(std::move(items));
        }
        dictionary_values.push_back(std::move(values));
    });

    auto wiring = bg::GraphWiring{};
    auto list_output = bg::WiringPoint{};
    auto bundle_output = bg::WiringPoint{};
    auto dictionary_output = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        list_output = list_queue();
        bundle_output = bundle_queue();
        dictionary_output = dictionary_queue();
        list_sink(list_output);
        bundle_sink(bundle_output);
        dictionary_sink(dictionary_output);
    }
    wiring.finalize();
    const auto list_target = wiring.output_node_index(list_output);
    const auto bundle_target = wiring.output_node_index(bundle_output);
    const auto dictionary_target = wiring.output_node_index(dictionary_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    auto list = runtime.make_queue(list_target);
    auto bundle = runtime.make_queue(bundle_target);
    auto dictionary = runtime.make_queue(dictionary_target);
    REQUIRE(list.write(::literal::TimeSeriesLiteral{{0, 1}, {1, 10}}));
    REQUIRE(list.write(::literal::TimeSeriesLiteral{{0, 2}, {1, 20}}));
    REQUIRE(bundle.write(::literal::TimeSeriesLiteral{{"first", 3}, {"second", 30}}));
    REQUIRE(bundle.write(::literal::TimeSeriesLiteral{{"first", 4}, {"second", 40}}));
    REQUIRE(dictionary.write(::literal::TimeSeriesLiteral{{1, 5}, {2, 50}}));
    REQUIRE(dictionary.write(::literal::TimeSeriesLiteral{{1, 6}, {2, 60}}));
    runtime.evaluate(time(2));

    CHECK(list_values == std::vector<std::vector<std::vector<bg::i64>>>{{{1, 2}, {10, 20}}});
    CHECK(bundle_values == std::vector<std::vector<std::vector<bg::i64>>>{{{3, 4}, {30, 40}}});
    CHECK(dictionary_values == std::vector<std::vector<std::vector<bg::i64>>>{{{5, 6}, {50, 60}}});

    REQUIRE(list.write(::literal::TimeSeriesLiteral{{0, 7}}));
    REQUIRE(bundle.write(::literal::TimeSeriesLiteral{{"first", 8}}));
    REQUIRE(dictionary.write(::literal::TimeSeriesLiteral{{1, 9}}));
    runtime.evaluate(time(3));

    CHECK(list_values == std::vector<std::vector<std::vector<bg::i64>>>{{{1, 2}, {10, 20}}, {{7}, {10, 20}}});
    CHECK(bundle_values == std::vector<std::vector<std::vector<bg::i64>>>{{{3, 4}, {30, 40}}, {{8}, {30, 40}}});
    CHECK(dictionary_values == std::vector<std::vector<std::vector<bg::i64>>>{{{5, 6}, {50, 60}}, {{9}, {50, 60}}});
    runtime.stop();
}

TEST_CASE("batch queue admission keeps conflicting bundle frames pending as a whole",
          "[evaluation][feature:time-series-storage][series:tsb][scalar:list][scalar:i64][queue][batch][failure]") {
    using Values = std::vector<bg::i64>;
    using Event = std::pair<std::optional<Values>, std::optional<Values>>;
    auto events = std::vector<Event>{};
    const auto queue_node = bg::queue<bg::TSB<BatchQueueBundle>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto sink = bg::node([&](bg::In<bg::TSB<BatchQueueBundle>> input) {
        const auto values = [](const auto &field) -> std::optional<Values> {
            if (!field.valid()) {
                return std::nullopt;
            }
            auto result = Values{};
            const auto list = field.value_view();
            for (auto index = std::size_t{}; index < list.size(); ++index) {
                result.push_back(list[index].value());
            }
            return result;
        };
        events.emplace_back(values(input->first), values(input->second));
    });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{"first", 1}, {"second", 10}}));
    runtime.evaluate(time(2));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{"second", ::literal::INVALID}}));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{"first", 2}, {"second", 20}}));
    runtime.evaluate(time(3));

    CHECK(events == std::vector<Event>{{Values{1}, Values{10}}, {Values{1}, std::nullopt}});

    runtime.evaluate(time(4));
    CHECK(events == std::vector<Event>{{Values{1}, Values{10}}, {Values{1}, std::nullopt}, {Values{2}, Values{20}}});
    runtime.stop();
}

TEST_CASE("batch queue invalidations wait for changes and redundant invalidations remain no-ops",
          "[evaluation][feature:time-series-storage][series:value][scalar:list][scalar:i64][queue][batch]"
          "[transition:invalidation]") {
    using Values = std::vector<bg::i64>;
    auto events = std::vector<std::optional<Values>>{};
    const auto queue_node = bg::queue<bg::TS<bg::List<bg::i64>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto sink = bg::node([&](bg::In<bg::TS<bg::List<bg::i64>>, bg::optional> input) {
        if (!input.valid()) {
            events.emplace_back(std::nullopt);
            return;
        }
        auto values = Values{};
        const auto list = input.value_view();
        for (auto index = std::size_t{}; index < list.size(); ++index) {
            values.push_back(list[index].value());
        }
        events.emplace_back(std::move(values));
    });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{10})));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    runtime.evaluate(time(2));
    CHECK(events == std::vector<std::optional<Values>>{Values{10}});

    runtime.evaluate(time(3));
    CHECK(events == std::vector<std::optional<Values>>{Values{10}, std::nullopt});

    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{20})));
    runtime.evaluate(time(4));
    CHECK(events == std::vector<std::optional<Values>>{Values{10}, std::nullopt, Values{20}});

    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{30})));
    runtime.evaluate(time(5));
    CHECK(events == std::vector<std::optional<Values>>{Values{10}, std::nullopt, Values{20}, std::nullopt});

    runtime.evaluate(time(6));
    CHECK(events == std::vector<std::optional<Values>>{Values{10}, std::nullopt, Values{20}, std::nullopt, Values{30}});
    runtime.stop();
}

TEST_CASE("batch dictionary admission preserves per-key removal and recreation boundaries",
          "[evaluation][feature:time-series-storage][series:tsd][scalar:list][scalar:i64][queue][batch]"
          "[transition:removal][transition:addition]") {
    using Values = std::vector<bg::i64>;
    using Event = std::pair<std::optional<Values>, std::optional<Values>>;
    auto events = std::vector<Event>{};
    const auto queue_node = bg::queue<bg::TSD<bg::i64, bg::TS<bg::List<bg::i64>>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto sink = bg::node([&](bg::In<bg::TSD<bg::i64, bg::TS<bg::List<bg::i64>>>> input) {
        const auto values = [&](bg::i64 key) -> std::optional<Values> {
            if (!input.contains(key)) {
                return std::nullopt;
            }

            const auto item = input[key];
            if (!item.valid()) {
                return std::nullopt;
            }
            auto result = Values{};
            const auto list = item.value_view();
            for (auto index = std::size_t{}; index < list.size(); ++index) {
                result.push_back(list[index].value());
            }
            return result;
        };
        events.emplace_back(values(1), values(2));
    });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 10}, {2, 100}}));
    runtime.evaluate(time(2));

    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 11}}));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, ::literal::REMOVE}}));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{2, 101}}));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 12}}));
    runtime.evaluate(time(3));
    CHECK(events == std::vector<Event>{{Values{10}, Values{100}}, {Values{11}, Values{100}}});

    runtime.evaluate(time(4));
    CHECK(events == std::vector<Event>{{Values{10}, Values{100}}, {Values{11}, Values{100}},
                                        {std::nullopt, Values{101}}});

    runtime.evaluate(time(5));
    CHECK(events == std::vector<Event>{{Values{10}, Values{100}}, {Values{11}, Values{100}},
                                        {std::nullopt, Values{101}}, {Values{12}, Values{101}}});

    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{9, ::literal::REMOVE}}));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{9, 90}}));
    runtime.evaluate(time(6));
    CHECK(events == std::vector<Event>{{Values{10}, Values{100}}, {Values{11}, Values{100}},
                                        {std::nullopt, Values{101}}, {Values{12}, Values{101}},
                                        {Values{12}, Values{101}}});
    runtime.stop();
}

TEST_CASE("batch queue replay preserves owned scalar values in the output list",
          "[evaluation][feature:time-series-storage][series:value][scalar:list][scalar:string][queue][batch]") {
    auto batch_values = std::vector<std::vector<std::string>>{};
    const auto queue_node = bg::queue<bg::TS<bg::List<bg::string>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto sink = bg::node([&](bg::In<bg::TS<bg::List<bg::string>>> input) {
        auto values = std::vector<std::string>{};
        const auto list = input.value_view();
        for (auto index = std::size_t{}; index < list.size(); ++index) {
            values.emplace_back(list[index].value());
        }
        batch_values.push_back(std::move(values));
    });

    auto wiring = bg::GraphWiring{};
    auto output = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral("first owned string value with allocated storage")));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral("second owned string value with allocated storage")));
    runtime.evaluate(time(2));
    CHECK(batch_values == std::vector<std::vector<std::string>>{{
                              "first owned string value with allocated storage",
                              "second owned string value with allocated storage"}});

    REQUIRE(queue.write(::literal::TimeSeriesLiteral("next owned string value with allocated storage")));
    runtime.evaluate(time(3));
    CHECK(batch_values == std::vector<std::vector<std::string>>{{
                              "first owned string value with allocated storage",
                              "second owned string value with allocated storage"},
                          {"next owned string value with allocated storage"}});
    runtime.stop();
}

TEST_CASE("batch queue retains owned payloads while an invalidated leaf waits pending",
          "[evaluation][feature:time-series-storage][series:value][scalar:compound][scalar:list][queue][batch]"
          "[lifetime][transition:invalidation]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    auto values = std::vector<std::vector<bg::i64>>{};
    const auto queue_node = bg::queue<bg::TS<bg::List<QueuePayload>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto sink = bg::node([&](bg::In<bg::TS<bg::List<QueuePayload>>> input) {
        auto items = std::vector<bg::i64>{};
        const auto list = input.value_view();
        for (auto index = std::size_t{}; index < list.size(); ++index) {
            items.push_back(list[index].value().number);
        }
        values.push_back(std::move(items));
    });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    auto initial_payload = std::make_shared<int>(1);
    auto initial_probe = std::weak_ptr<int>{initial_payload};
    QueuePayloadBufferHandlers::writing_payload = initial_payload;
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(QueuePayload{1})));
    QueuePayloadBufferHandlers::writing_payload.reset();
    initial_payload.reset();
    CHECK_FALSE(initial_probe.expired());
    runtime.evaluate(time(2));
    CHECK(initial_probe.expired());
    CHECK(values == std::vector<std::vector<bg::i64>>{{1}});

    auto pending_payload = std::make_shared<int>(2);
    auto pending_probe = std::weak_ptr<int>{pending_payload};
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    QueuePayloadBufferHandlers::writing_payload = pending_payload;
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(QueuePayload{2})));
    QueuePayloadBufferHandlers::writing_payload.reset();
    pending_payload.reset();
    CHECK_FALSE(pending_probe.expired());

    runtime.evaluate(time(3));
    CHECK_FALSE(pending_probe.expired());
    CHECK(values == std::vector<std::vector<bg::i64>>{{1}});

    runtime.evaluate(time(4));
    CHECK(pending_probe.expired());
    CHECK(values == std::vector<std::vector<bg::i64>>{{1}, {2}});
    runtime.stop();
}

TEST_CASE("a standard queue conflict retains its frame and stops later targets",
          "[evaluation][feature:time-series-storage][series:tsd][series:value][scalar:i64][queue][failure]") {
    auto dictionary_values = std::vector<bg::i64>{};
    auto scalar_values = std::vector<bg::i64>{};
    const auto dictionary_queue = bg::queue<bg::TSD<bg::i64, bg::TS<bg::i64>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto scalar_queue = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto dictionary_sink = bg::node([&](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        dictionary_values.push_back(input.contains(1) ? *input[1] : -1);
    });
    const auto scalar_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { scalar_values.push_back(*input); });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint dictionary_output;
    bg::WiringPoint scalar_output;
    {
        const auto context = bg::WiringContext{wiring};
        dictionary_output = dictionary_queue();
        scalar_output = scalar_queue();
        dictionary_sink(dictionary_output);
        scalar_sink(scalar_output);
    }
    wiring.finalize();
    const auto dictionary_target = wiring.output_node_index(dictionary_output);
    const auto scalar_target = wiring.output_node_index(scalar_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    auto dictionary = runtime.make_queue(dictionary_target);
    auto scalar = runtime.make_queue(scalar_target);
    REQUIRE(dictionary.write(::literal::TimeSeriesLiteral{{1, 10}}));
    REQUIRE(dictionary.write(::literal::TimeSeriesLiteral{{1, ::literal::REMOVE}}));
    REQUIRE(scalar.write(::literal::TimeSeriesLiteral(bg::i64{20})));

    runtime.evaluate(time(2));
    CHECK(dictionary_values == std::vector<bg::i64>{10});
    CHECK(scalar_values.empty());

    runtime.evaluate(time(3));
    CHECK(dictionary_values == std::vector<bg::i64>{10, -1});
    CHECK(scalar_values == std::vector<bg::i64>{20});
    runtime.stop();
}

TEST_CASE("batch queue values after a standard conflict start the next cycle with a fresh list",
          "[evaluation][feature:time-series-storage][series:value][scalar:list][scalar:i64][queue][batch][failure]") {
    auto batch_values = std::vector<std::vector<bg::i64>>{};
    auto standard_values = std::vector<bg::i64>{};
    const auto batch_queue = bg::queue<bg::TS<bg::List<bg::i64>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::batch);
    const auto standard_queue = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto batch_sink = bg::node([&](bg::In<bg::TS<bg::List<bg::i64>>> input) {
        auto values = std::vector<bg::i64>{};
        const auto list = input.value_view();
        for (auto index = std::size_t{}; index < list.size(); ++index) {
            values.push_back(list[index].value());
        }
        batch_values.push_back(std::move(values));
    });
    const auto standard_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) {
        standard_values.push_back(*input);
    });

    auto wiring = bg::GraphWiring{};
    auto batch_output = bg::WiringPoint{};
    auto standard_output = bg::WiringPoint{};
    {
        const auto context = bg::WiringContext{wiring};
        batch_output = batch_queue();
        standard_output = standard_queue();
        batch_sink(batch_output);
        standard_sink(standard_output);
    }
    wiring.finalize();
    const auto batch_target = wiring.output_node_index(batch_output);
    const auto standard_target = wiring.output_node_index(standard_output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));

    auto batch = runtime.make_queue(batch_target);
    auto standard = runtime.make_queue(standard_target);
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{10})));
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{20})));
    REQUIRE(standard.write(::literal::TimeSeriesLiteral(bg::i64{1})));
    REQUIRE(standard.write(::literal::TimeSeriesLiteral(bg::i64{2})));
    REQUIRE(batch.write(::literal::TimeSeriesLiteral(bg::i64{30})));
    runtime.evaluate(time(2));

    CHECK(batch_values == std::vector<std::vector<bg::i64>>{{10, 20}});
    CHECK(standard_values == std::vector<bg::i64>{1});

    runtime.evaluate(time(3));
    CHECK(batch_values == std::vector<std::vector<bg::i64>>{{10, 20}, {30}});
    CHECK(standard_values == std::vector<bg::i64>{1, 2});
    runtime.stop();
}

TEST_CASE("queue dictionary updates, removal, no-op removal, and recreation publish current-cycle changes",
          "[evaluation][feature:time-series-storage][feature:collection-change][series:tsd][scalar:i64]"
          "[queue][transition:update][transition:removal]") {
    struct Event {
        bg::i64 value;
        std::size_t added;
        std::size_t removed;
        std::size_t modified;
        bg::EvaluationTime time;
    };
    auto events = std::vector<Event>{};
    const auto queue_node = bg::queue<bg::TSD<bg::i64, bg::TS<bg::i64>>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto sink = bg::node([&](bg::In<bg::TSD<bg::i64, bg::TS<bg::i64>>> input) {
        auto added = std::size_t{};
        auto removed = std::size_t{};
        auto modified = std::size_t{};
        input.for_each_added([&](const auto &) { ++added; });
        input.for_each_removed([&](const auto &) { ++removed; });
        input.for_each_modified([&](const auto &) { ++modified; });
        events.push_back({input.contains(1) ? *input[1] : -1, added, removed, modified, input.last_updated()});
    });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        sink(output);
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 10}}));
    runtime.evaluate(time(2));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 11}}));
    runtime.evaluate(time(3));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, ::literal::REMOVE}}));
    runtime.evaluate(time(4));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, ::literal::REMOVE}}));
    runtime.evaluate(time(5));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral{{1, 12}}));
    runtime.evaluate(time(6));

    REQUIRE(events.size() == 4);
    CHECK(events[0].value == 10);
    CHECK(events[0].added == 1);
    CHECK(events[0].time == time(2));
    CHECK(events[1].value == 11);
    CHECK(events[1].modified == 1);
    CHECK(events[1].time == time(3));
    CHECK(events[2].value == -1);
    CHECK(events[2].removed == 1);
    CHECK(events[2].time == time(4));
    CHECK(events[3].value == 12);
    CHECK(events[3].added == 1);
    CHECK(events[3].time == time(6));
    runtime.stop();
}

TEST_CASE("queued output changes notify required, optional, and reference subscribers",
          "[evaluation][feature:time-series-storage][feature:reference][feature:validity][series:value]"
          "[queue][scalar:i64][validity:optional][transition:invalidation][transition:revalidation]") {
    auto required_values = std::vector<bg::i64>{};
    auto optional_values = std::vector<std::optional<bg::i64>>{};
    auto reference_values = std::vector<bg::i64>{};
    const auto queue_node = bg::queue<bg::TS<bg::i64>>(
        bg::start = [](bg::Queue) {}, bg::mode = bg::QueueMode::standard);
    const auto required_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { required_values.push_back(*input); });
    const auto optional_sink = bg::node([&](bg::In<bg::TS<bg::i64>, bg::optional> input) {
        optional_values.push_back(input.valid() ? std::optional<bg::i64>{*input} : std::nullopt);
    });
    const auto reference_pass = bg::node([](bg::In<bg::REF<bg::TS<bg::i64>>> input,
                                           bg::Out<bg::REF<bg::TS<bg::i64>>> output) {
        output.set_value(input.value());
    });
    const auto reference_sink = bg::node([&](bg::In<bg::TS<bg::i64>> input) { reference_values.push_back(*input); });

    auto wiring = bg::GraphWiring{};
    bg::WiringPoint output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
        required_sink(output);
        optional_sink(output);
        reference_sink(reference_pass(output));
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{10})));
    runtime.evaluate(time(2));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(::literal::INVALID)));
    runtime.evaluate(time(3));
    REQUIRE(queue.write(::literal::TimeSeriesLiteral(bg::i64{11})));
    runtime.evaluate(time(4));

    CHECK(required_values == std::vector<bg::i64>{10, 11});
    CHECK(optional_values == std::vector<std::optional<bg::i64>>{10, std::nullopt, 11});
    CHECK(reference_values == std::vector<bg::i64>{10, 11});
    runtime.stop();
}

TEST_CASE("queue replay failure releases typed unread payloads after stop callbacks",
          "[evaluation][feature:time-series-storage][series:value][scalar:compound][queue][failure][lifetime]") {
    REQUIRE(QueuePayloadBufferHandlers::register_operations());
    QueuePayloadBufferHandlers::read_calls = 0;
    QueuePayloadBufferHandlers::fail_on_read = 0;
    auto third_payload = std::weak_ptr<int>{};
    auto third_payload_alive_during_stop = false;
    const auto queue_node = bg::queue<bg::TS<QueuePayload>>(
        bg::start = [](bg::Queue) {},
        bg::stop = [&]() noexcept { third_payload_alive_during_stop = !third_payload.expired(); },
        bg::mode = bg::QueueMode::elide);
    auto wiring = bg::GraphWiring{};
    std::optional<bg::WiringPoint> output;
    {
        const auto context = bg::WiringContext{wiring};
        output = queue_node();
    }
    wiring.finalize();
    const auto target = wiring.output_node_index(*output);
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    runtime.start(time(1));
    auto queue = runtime.make_queue(target);

    const auto write_payload = [&](bg::i64 number, std::weak_ptr<int> *probe = nullptr) {
        auto payload = std::make_shared<int>(static_cast<int>(number));
        if (probe != nullptr) {
            *probe = payload;
        }
        QueuePayloadBufferHandlers::writing_payload = payload;
        auto value = bg::ScalarValue<QueuePayload>{QueuePayload{number}};
        const auto accepted = queue.write(::literal::TimeSeriesLiteral(value));
        QueuePayloadBufferHandlers::writing_payload.reset();
        return accepted;
    };
    REQUIRE(write_payload(1));
    REQUIRE(write_payload(2));
    REQUIRE(write_payload(3, &third_payload));
    QueuePayloadBufferHandlers::fail_on_read = 2;

    CHECK_THROWS_AS(runtime.evaluate(time(2)), std::runtime_error);
    CHECK(third_payload_alive_during_stop);
    CHECK(third_payload.expired());
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(::literal::REMOVE)));
    QueuePayloadBufferHandlers::fail_on_read = 0;
}
