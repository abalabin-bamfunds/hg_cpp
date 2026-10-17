#include <bg/details/engine_wakeup.hpp>
#include <bg/periodic_callback.hpp>
#include <bg/evaluation_profile.hpp>
#include <bg/graph/compiled_graph.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/queue_node.hpp>
#include <bg/realtime_evaluation_engine.hpp>
#include <bg/wall_clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct QueueState {
    static void set(bg::Queue value) {
        std::scoped_lock lock{mutex};
        queue = std::move(value);
        ready = true;
        changed.notify_all();
    }

    static bg::Queue copy() {
        std::scoped_lock lock{mutex};
        return queue;
    }

    static bool is_ready() {
        std::scoped_lock lock{mutex};
        return ready;
    }

    static void reset() {
        std::scoped_lock lock{mutex};
        queue = {};
        ready = false;
        changed.notify_all();
    }

    inline static std::mutex mutex;
    inline static std::condition_variable changed;
    inline static bg::Queue queue;
    inline static bool ready{};
};

struct QueueStateScope {
    ~QueueStateScope() { QueueState::reset(); }
};

struct RealtimeProbe {
    static constexpr bool uses_timed_scheduling = true;

    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const {
        const auto wall_base = context->wall_clock().now();
        std::scoped_lock lock{mutex};
        ++start_count;
        context->schedule_now();
        for (const auto offset : exact_offsets) {
            context->schedule_at(context->evaluation_time() + offset);
        }
        for (const auto offset : wall_offsets) {
            const auto deadline = wall_base + offset;
            wall_deadlines.push_back(deadline);
            wall_handles.push_back(context->schedule_wall_clock_at(deadline));
        }
        changed.notify_all();
    }

    void evaluate(bg::EvaluationContext *context) const {
        auto value = bg::i64{};
        auto write_queue = false;
        auto schedule_exact = false;
        auto delay = std::chrono::milliseconds{};
        {
            std::unique_lock lock{mutex};
            delay = evaluation_delay;
            evaluation_times.push_back(context->evaluation_time());
            value = static_cast<bg::i64>(evaluation_times.size());
            if (queue_on_first_evaluation && !queue_written) {
                queue_written = true;
                write_queue = true;
            }
            if (reschedules_remaining != 0) {
                --reschedules_remaining;
                schedule_exact = true;
            }
            if (!wall_handles.empty()) {
                auto fired = std::vector<bool>{};
                fired.reserve(wall_handles.size());
                for (const auto handle : wall_handles) {
                    fired.push_back(context->timer_fired(handle));
                }
                wall_observations.push_back({context->wall_clock().now(), std::move(fired)});
            }
            changed.notify_all();
            if (block_evaluation) {
                entered = true;
                changed.notify_all();
                changed.wait(lock, [] { return release; });
            }
            if (throw_on_evaluation) {
                throw std::runtime_error{"realtime probe evaluation failure"};
            }
        }

        std::this_thread::sleep_for(delay);
        output.set(value);
        if (write_queue) {
            (void)QueueState::copy().write(::literal::TimeSeriesLiteral(bg::i64{42}));
        }
        if (schedule_exact) {
            context->schedule_at(context->evaluation_time() + reschedule_interval);
        }
    }

    void stop() const noexcept {
        std::scoped_lock lock{mutex};
        ++stop_count;
        changed.notify_all();
    }

    static void release_evaluation() {
        {
            std::scoped_lock lock{mutex};
            release = true;
        }
        changed.notify_all();
    }

    static void reset() {
        std::scoped_lock lock{mutex};
        exact_offsets.clear();
        reschedule_interval = std::chrono::microseconds{};
        evaluation_delay = std::chrono::milliseconds{};
        reschedules_remaining = 0;
        wall_offsets.clear();
        evaluation_times.clear();
        wall_deadlines.clear();
        wall_handles.clear();
        wall_observations.clear();
        start_count = 0;
        stop_count = 0;
        queue_on_first_evaluation = false;
        queue_written = false;
        throw_on_evaluation = false;
        block_evaluation = false;
        entered = false;
        release = false;
    }

    struct WallObservation {
        bg::EvaluationTime now;
        std::vector<bool> fired;
    };

    inline static std::mutex mutex;
    inline static std::condition_variable changed;
    inline static std::vector<std::chrono::microseconds> exact_offsets;
    inline static std::chrono::microseconds reschedule_interval{};
    inline static std::chrono::milliseconds evaluation_delay{};
    inline static std::vector<std::chrono::microseconds> wall_offsets;
    inline static std::vector<bg::EvaluationTime> evaluation_times;
    inline static std::vector<bg::EvaluationTime> wall_deadlines;
    inline static std::vector<bg::ScheduleHandle> wall_handles;
    inline static std::vector<WallObservation> wall_observations;
    inline static std::size_t start_count{};
    inline static std::size_t stop_count{};
    inline static std::size_t reschedules_remaining{};
    inline static bool queue_on_first_evaluation{};
    inline static bool queue_written{};
    inline static bool throw_on_evaluation{};
    inline static bool block_evaluation{};
    inline static bool entered{};
    inline static bool release{};
};

struct QueueSink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate(bg::EvaluationContext *context) const {
        std::scoped_lock lock{mutex};
        values.emplace_back(context->evaluation_time(), *input);
        changed.notify_all();
    }

    inline static std::mutex mutex;
    inline static std::condition_variable changed;
    inline static std::vector<std::pair<bg::EvaluationTime, bg::i64>> values;
};

struct QueueChainSink {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<bg::TS<bg::i64>> input;

    void evaluate(bg::EvaluationContext *context) const {
        auto schedule_next = false;
        {
            std::scoped_lock lock{mutex};
            const auto timer_fired = context->timer_fired(timer);
            events.push_back(timer_fired ? 0 : static_cast<int>(*input));
            if (timer_fired) {
                ++timer_count;
                schedule_next = timer_count < 3;
            } else {
                schedule_next = *input == 1;
            }
            changed.notify_all();
        }

        if (schedule_next) {
            timer = context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    static void reset() {
        std::scoped_lock lock{mutex};
        events.clear();
        timer = {};
        timer_count = 0;
    }

    inline static std::mutex mutex;
    inline static std::condition_variable changed;
    inline static std::vector<int> events;
    inline static bg::ScheduleHandle timer;
    inline static std::size_t timer_count{};
};

struct DownstreamSink {
    bg::In<bg::TS<bg::i64>> input;

    void evaluate() const {
        std::scoped_lock lock{mutex};
        ++count;
        changed.notify_all();
    }

    inline static std::mutex mutex;
    inline static std::condition_variable changed;
    inline static std::size_t count{};
};

class EngineWorker {
  public:
    template <typename Run> explicit EngineWorker(Run run)
        : thread_{[this, run = std::move(run)]() mutable {
              try {
                  run(stop_source_.get_token());
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }} {}

    EngineWorker(const EngineWorker &) = delete;
    EngineWorker &operator=(const EngineWorker &) = delete;

    ~EngineWorker() {
        RealtimeProbe::release_evaluation();
        stop_source_.request_stop();
        join();
    }

    void request_stop() noexcept { stop_source_.request_stop(); }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::exception_ptr failure() const noexcept { return failure_; }

    void rethrow_failure() const {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

  private:
    std::stop_source stop_source_;
    std::exception_ptr failure_;
    std::jthread thread_;
};

template <typename Predicate>
bool wait_for(std::condition_variable &changed, std::mutex &mutex, Predicate predicate,
              std::chrono::milliseconds timeout = std::chrono::seconds{3}) {
    std::unique_lock lock{mutex};
    return changed.wait_for(lock, timeout, predicate);
}

struct ProbeGraph {
    decltype(bg::node<RealtimeProbe>()) probe = bg::node<RealtimeProbe>();
    decltype(bg::node<DownstreamSink>()) sink = bg::node<DownstreamSink>();
    bg::GraphWiring wiring;

    ProbeGraph() {
        auto context = bg::WiringContext{wiring};
        sink(probe());
        wiring.finalize();
    }
};

struct QueueGraph {
    inline static constexpr auto start_queue = [](bg::Queue writer) { QueueState::set(std::move(writer)); };
    using QueueNode = decltype(bg::queue<bg::TS<bg::i64>>(bg::start = start_queue));

    QueueNode queue = bg::queue<bg::TS<bg::i64>>(bg::start = start_queue);
    decltype(bg::node<QueueSink>()) queue_sink = bg::node<QueueSink>();
    decltype(bg::node<RealtimeProbe>()) probe = bg::node<RealtimeProbe>();
    bg::GraphWiring wiring;

    QueueGraph() {
        auto context = bg::WiringContext{wiring};
        queue_sink(queue());
        (void)probe();
        wiring.finalize();
    }
};

void verify_queue_wakes_idle_engine(std::optional<std::chrono::microseconds> exact_timer_offset) {
    RealtimeProbe::reset();
    QueueState::reset();
    QueueSink::values.clear();
    if (exact_timer_offset) {
        RealtimeProbe::exact_offsets = {*exact_timer_offset};
    }
    QueueGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};
    const auto start = bg::SystemWallClock::instance().now();
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, start, token);
    }};

    const auto initial_cycle = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return RealtimeProbe::evaluation_times.size() == 1;
    });
    REQUIRE(initial_cycle);
    const auto timer_fired_early = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return RealtimeProbe::evaluation_times.size() > 1;
    }, std::chrono::milliseconds{20});
    REQUIRE_FALSE(timer_fired_early);

    auto queue = QueueState::copy();
    const auto accepted = queue.write(::literal::TimeSeriesLiteral(bg::i64{73}));
    const auto received = wait_for(QueueSink::changed, QueueSink::mutex, [] { return !QueueSink::values.empty(); });
    worker.request_stop();
    worker.join();

    CHECK(accepted);
    REQUIRE(received);
    REQUIRE_FALSE(worker.failure());
    CHECK(QueueSink::values.front().second == 73);
    CHECK(QueueSink::values.front().first >= start);
    if (exact_timer_offset) {
        CHECK(QueueSink::values.front().first < start + *exact_timer_offset);
    }
    CHECK(RealtimeProbe::evaluation_times.size() == 1);
    CHECK_FALSE(queue.write(::literal::TimeSeriesLiteral(bg::i64{74})));
}

} // namespace

TEST_CASE("realtime exact cycles preserve deadlines before pending queue input",
          "[evaluation][realtime][feature:realtime][feature:timed-scheduling][queue][feature:profiling]") {
    RealtimeProbe::reset();
    QueueState::reset();
    QueueSink::values.clear();
    DownstreamSink::count = 0;
    RealtimeProbe::reschedule_interval = std::chrono::minutes{20};
    RealtimeProbe::reschedules_remaining = 2;
    RealtimeProbe::queue_on_first_evaluation = true;
    QueueGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};
    auto profile = bg::EvaluationProfile{};
    const auto start = bg::SystemWallClock::instance().now() - std::chrono::hours{1};
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, start, profile, token);
    }};

    const auto received = wait_for(QueueSink::changed, QueueSink::mutex, [] { return !QueueSink::values.empty(); });
    worker.request_stop();
    worker.join();
    profile.finish();

    REQUIRE(received);
    REQUIRE_FALSE(worker.failure());
    CHECK(RealtimeProbe::evaluation_times == std::vector{start, start + std::chrono::minutes{20},
                                                         start + std::chrono::minutes{40}});
    REQUIRE(QueueSink::values.size() == 1);
    CHECK(QueueSink::values.front().first > start + std::chrono::minutes{40});
    CHECK(QueueSink::values.front().second == 42);

    auto reader = bg::EvaluationProfileReader{profile.take_recording()};
    auto cycle_times = std::vector<bg::EvaluationTime>{};
    while (const auto record = reader.read()) {
        if (const auto *cycle = std::get_if<bg::EvaluationProfileCycle>(&*record);
            cycle != nullptr && cycle->boundary == bg::EvaluationProfileCycle::Boundary::start) {
            cycle_times.push_back(cycle->evaluation_time);
        }
    }
    CHECK(cycle_times == std::vector{start, start + std::chrono::minutes{20},
                                     start + std::chrono::minutes{40}, QueueSink::values.front().first});
    CHECK(RealtimeProbe::stop_count == 1);
    QueueState::reset();
}

TEST_CASE("queue input wakes an idle realtime engine with and without an exact timer",
          "[evaluation][realtime][feature:realtime][feature:timed-scheduling][queue][feature:time-series-storage]") {
    SECTION("no exact timer") { verify_queue_wakes_idle_engine(std::nullopt); }
    SECTION("one-hour exact timer") {
        verify_queue_wakes_idle_engine(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::hours{1}));
    }
}

TEST_CASE("a conflicting standard queue message waits through a dynamically rescheduled timer chain",
          "[evaluation][realtime][feature:realtime][feature:timed-scheduling][queue][feature:time-series-storage]") {
    RealtimeProbe::reset();
    QueueState::reset();
    QueueChainSink::reset();
    auto writes_accepted = false;
    const auto queue = bg::queue<bg::TS<bg::i64>>(bg::start = [&](bg::Queue writer) {
        writes_accepted = writer.write(::literal::TimeSeriesLiteral(bg::i64{1})) &&
                          writer.write(::literal::TimeSeriesLiteral(bg::i64{2}));
        QueueState::set(std::move(writer));
    });
    const auto sink = bg::node<QueueChainSink>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        sink(queue());
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::SystemWallClock::instance().now(), token);
    }};

    const auto complete = wait_for(QueueChainSink::changed, QueueChainSink::mutex, [] {
        return QueueChainSink::events.size() == 5;
    });
    worker.request_stop();
    worker.join();
    worker.rethrow_failure();

    REQUIRE(complete);
    CHECK(writes_accepted);
    CHECK(QueueChainSink::events == std::vector{1, 0, 0, 0, 2});
    CHECK_FALSE(QueueState::copy().write(::literal::TimeSeriesLiteral(bg::i64{3})));
}

TEST_CASE("realtime wall-clock timers run after their wall deadline",
          "[evaluation][realtime][feature:realtime][feature:timed-scheduling][feature:wall-clock]") {
    RealtimeProbe::reset();
    RealtimeProbe::wall_offsets = {-std::chrono::microseconds{2}, -std::chrono::microseconds{1}};
    ProbeGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}, token);
    }};

    const auto fired = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return std::ranges::any_of(RealtimeProbe::wall_observations, [](const auto &observation) {
            return std::ranges::all_of(observation.fired, [](bool value) { return value; });
        });
    });
    worker.request_stop();
    worker.join();

    REQUIRE(fired);
    REQUIRE_FALSE(worker.failure());
    REQUIRE(RealtimeProbe::wall_deadlines.size() == 2);
    const auto fired_observation = std::ranges::find_if(RealtimeProbe::wall_observations, [](const auto &value) {
        return std::ranges::all_of(value.fired, [](bool timer_fired) { return timer_fired; });
    });
    REQUIRE(fired_observation != RealtimeProbe::wall_observations.end());
    CHECK(std::ranges::all_of(RealtimeProbe::wall_deadlines, [&](const auto deadline) {
        return fired_observation->now >= deadline;
    }));
}

TEST_CASE("realtime evaluation waits until a future wall-clock deadline",
          "[evaluation][realtime][feature:realtime][feature:timed-scheduling][feature:wall-clock]") {
    RealtimeProbe::reset();
    RealtimeProbe::wall_offsets = {std::chrono::milliseconds{20}};
    ProbeGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}, token);
    }};

    const auto fired = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return std::ranges::any_of(RealtimeProbe::wall_observations, [](const auto &observation) {
            return std::ranges::any_of(observation.fired, [](bool value) { return value; });
        });
    });
    worker.request_stop();
    worker.join();

    REQUIRE(fired);
    REQUIRE_FALSE(worker.failure());
    REQUIRE(RealtimeProbe::wall_deadlines.size() == 1);
    const auto fired_observation = std::ranges::find_if(RealtimeProbe::wall_observations, [](const auto &value) {
        return std::ranges::any_of(value.fired, [](bool timer_fired) { return timer_fired; });
    });
    REQUIRE(fired_observation != RealtimeProbe::wall_observations.end());
    CHECK(fired_observation->now >= RealtimeProbe::wall_deadlines.front());
}

TEST_CASE("realtime stop interrupts a wait for a future wall-clock timer",
          "[evaluation][realtime][feature:realtime][feature:wall-clock][feature:lifecycle]") {
    RealtimeProbe::reset();
    RealtimeProbe::wall_offsets = {std::chrono::hours{1}};
    ProbeGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}, token);
    }};

    const auto initial_cycle = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return RealtimeProbe::evaluation_times.size() == 1;
    });
    const auto timer_fired_early = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return RealtimeProbe::evaluation_times.size() > 1;
    }, std::chrono::milliseconds{20});
    worker.request_stop();
    worker.join();

    REQUIRE(initial_cycle);
    REQUIRE_FALSE(worker.failure());
    CHECK_FALSE(timer_fired_early);
    CHECK(RealtimeProbe::evaluation_times.size() == 1);
    CHECK(RealtimeProbe::stop_count == 1);
}

TEST_CASE("realtime engines stop while idle and reject pre-cancelled starts",
          "[evaluation][realtime][feature:realtime][feature:lifecycle]") {
    RealtimeProbe::reset();
    ProbeGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    std::stop_source already_stopped;
    already_stopped.request_stop();
    bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10},
                                       already_stopped.get_token());
    CHECK(RealtimeProbe::start_count == 0);

    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}, token);
    }};
    const auto initial_cycle = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
        return RealtimeProbe::evaluation_times.size() == 1;
    });
    worker.request_stop();
    worker.join();

    REQUIRE(initial_cycle);
    REQUIRE_FALSE(worker.failure());
    CHECK(RealtimeProbe::start_count == 1);
    CHECK(RealtimeProbe::stop_count == 1);
}

TEST_CASE("realtime periodic callback polls idle waits and stops after the completed cycle",
          "[evaluation][realtime][feature:realtime][feature:lifecycle][failure]") {
    RealtimeProbe::reset();
    ProbeGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto callback_calls = std::size_t{};
    const auto periodic_callback = bg::PeriodicCallback{
        .callback = [&] {
            if (++callback_calls == 2) {
                throw std::runtime_error{"periodic callback failed"};
            }
        },
        .interval = std::chrono::milliseconds{1},
    };
    const auto start = bg::SystemWallClock::instance().now();

    CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(runtime, start, std::stop_token{}, periodic_callback),
                    std::runtime_error);
    CHECK(callback_calls == 2);
    CHECK(RealtimeProbe::start_count == 1);
    CHECK(RealtimeProbe::evaluation_times == std::vector{start});
    CHECK(RealtimeProbe::stop_count == 1);
    CHECK(runtime.evaluation_time() == start);
}

TEST_CASE("realtime periodic callback queue writes are scheduled before waiting",
          "[evaluation][realtime][feature:realtime][queue]") {
    RealtimeProbe::reset();
    QueueState::reset();
    QueueSink::values.clear();
    RealtimeProbe::evaluation_delay = std::chrono::milliseconds{5};
    QueueGraph graph;
    const auto compiled = bg::CompiledGraph{graph.wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};
    auto callback_calls = std::size_t{};
    auto accepted = false;
    const auto periodic_callback = bg::PeriodicCallback{
        .callback = [&] {
            if (++callback_calls == 2) {
                accepted = QueueState::copy().write(::literal::TimeSeriesLiteral(bg::i64{73}));
            }
        },
        .interval = std::chrono::milliseconds{1},
    };
    EngineWorker worker{[&](std::stop_token token) {
        bg::RealtimeEvaluationEngine{}.run(runtime, bg::SystemWallClock::instance().now(), token, periodic_callback);
    }};

    const auto received = wait_for(QueueSink::changed, QueueSink::mutex, [] { return !QueueSink::values.empty(); });
    worker.request_stop();
    worker.join();

    REQUIRE(received);
    REQUIRE_FALSE(worker.failure());
    CHECK(accepted);
    CHECK(callback_calls >= 2);
    REQUIRE(QueueSink::values.size() == 1);
    CHECK(QueueSink::values.front().second == 73);
}

TEST_CASE("realtime periodic callbacks run during future startup without starting the graph",
          "[evaluation][realtime][feature:realtime][feature:lifecycle][failure]") {
    SECTION("polling the future-start wait") {
        RealtimeProbe::reset();
        ProbeGraph graph;
        const auto compiled = bg::CompiledGraph{graph.wiring};
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        auto callback_calls = std::size_t{};
        const auto periodic_callback = bg::PeriodicCallback{
            .callback = [&] {
                if (++callback_calls == 2) {
                    throw std::runtime_error{"periodic callback failed during future startup"};
                }
            },
            .interval = std::chrono::milliseconds{1},
        };
        const auto future_start = bg::SystemWallClock::instance().now() + std::chrono::hours{1};

        CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(runtime, future_start, std::stop_token{}, periodic_callback),
                        std::runtime_error);
        CHECK(callback_calls == 2);
        CHECK(RealtimeProbe::start_count == 0);
        CHECK(RealtimeProbe::evaluation_times.empty());
        CHECK(RealtimeProbe::stop_count == 0);
    }

    SECTION("an initial periodic callback") {
        RealtimeProbe::reset();
        ProbeGraph graph;
        const auto compiled = bg::CompiledGraph{graph.wiring};
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        auto callback_calls = std::size_t{};
        const auto periodic_callback = bg::PeriodicCallback{
            .callback = [&] {
                ++callback_calls;
                throw std::runtime_error{"periodic callback failed during startup"};
            },
        };

        CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(
                            runtime, bg::SystemWallClock::instance().now(), std::stop_token{}, periodic_callback),
                        std::runtime_error);
        CHECK(callback_calls == 1);
        CHECK(RealtimeProbe::start_count == 0);
        CHECK(RealtimeProbe::stop_count == 0);
    }
}

TEST_CASE("realtime stop cancels future startup and waits for the active cycle",
          "[evaluation][realtime][feature:realtime][feature:lifecycle]") {
    SECTION("cancellation during future startup") {
        RealtimeProbe::reset();
        ProbeGraph graph;
        const auto compiled = bg::CompiledGraph{graph.wiring};
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        std::binary_semaphore entered{0};
        std::binary_semaphore continue_to_run{0};
        EngineWorker worker{[&](std::stop_token token) {
            entered.release();
            continue_to_run.acquire();
            const auto future_start = bg::SystemWallClock::instance().now() + std::chrono::hours{24LL * 365 * 300};
            bg::RealtimeEvaluationEngine{}.run(runtime, future_start, token);
        }};
        entered.acquire();
        continue_to_run.release();
        const auto started_early = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
            return RealtimeProbe::start_count != 0;
        }, std::chrono::milliseconds{20});
        worker.request_stop();
        worker.join();

        REQUIRE_FALSE(worker.failure());
        CHECK_FALSE(started_early);
        CHECK(RealtimeProbe::start_count == 0);
        CHECK(RealtimeProbe::stop_count == 0);
    }

    SECTION("a stop request completes only after the running cycle reaches its downstream node") {
        RealtimeProbe::reset();
        DownstreamSink::count = 0;
        RealtimeProbe::block_evaluation = true;
        ProbeGraph graph;
        const auto compiled = bg::CompiledGraph{graph.wiring};
        auto global_state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{compiled, global_state};
        EngineWorker worker{[&](std::stop_token token) {
            bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}, token);
        }};
        const auto entered = wait_for(RealtimeProbe::changed, RealtimeProbe::mutex, [] {
            return RealtimeProbe::entered;
        });
        worker.request_stop();
        RealtimeProbe::release_evaluation();
        worker.join();

        REQUIRE(entered);
        REQUIRE_FALSE(worker.failure());
        CHECK(DownstreamSink::count == 1);
        CHECK(RealtimeProbe::stop_count == 1);
    }
}

TEST_CASE("realtime evaluation failure stops the graph and closes retained queues",
          "[evaluation][realtime][feature:realtime][feature:lifecycle][failure][queue][feature:time-series-storage]") {
    RealtimeProbe::reset();
    QueueState::reset();
    RealtimeProbe::throw_on_evaluation = true;
    const auto queue = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue writer) {
        QueueState::set(std::move(writer));
    });
    const auto probe = bg::node<RealtimeProbe>();
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)queue();
        (void)probe();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};

    CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}),
                    std::runtime_error);
    CHECK(RealtimeProbe::stop_count == 1);
    CHECK_FALSE(QueueState::copy().write(::literal::TimeSeriesLiteral(bg::i64{7})));
    QueueState::reset();
}

TEST_CASE("engine wakeup handles native clock range boundaries",
          "[coverage-exempt][realtime][wakeup]") {
    auto wakeup = bg::details::EngineWakeup{};
    const auto wait_for_wakeup = [&wakeup](auto deadline) {
        const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        do {
            if (wakeup.wait_until(deadline)) {
                return true;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < retry_deadline);
        return false;
    };

    wakeup.notify();
    const auto distant_future = bg::SystemWallClock::instance().now() + std::chrono::hours{24LL * 365 * 300};
    CHECK(wait_for_wakeup(distant_future));

    const auto distant_past = bg::SystemWallClock::instance().now() - std::chrono::hours{24LL * 365 * 1000};
    CHECK_FALSE(wakeup.wait_until(distant_past));
    wakeup.notify();
    CHECK(wait_for_wakeup(distant_past));
    CHECK_FALSE(wakeup.wait_until(distant_past));
}

TEST_CASE("realtime startup failure closes queue admission",
          "[evaluation][realtime][feature:realtime][feature:lifecycle][failure][queue][feature:time-series-storage]") {
    QueueState::reset();
    const auto queue = bg::queue<bg::TS<bg::i64>>(bg::start = [](bg::Queue writer) {
        QueueState::set(std::move(writer));
        throw std::runtime_error{"realtime queue startup failure"};
    });
    auto wiring = bg::GraphWiring{};
    {
        auto context = bg::WiringContext{wiring};
        (void)queue();
    }
    wiring.finalize();
    const auto compiled = bg::CompiledGraph{wiring};
    auto global_state = bg::GlobalState{};
    auto runtime = bg::RuntimeGraph{compiled, global_state};
    auto queue_state_scope = QueueStateScope{};

    CHECK_THROWS_AS(bg::RealtimeEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{10}),
                    std::runtime_error);
    CHECK(QueueState::is_ready());
    CHECK_FALSE(QueueState::copy().write(::literal::TimeSeriesLiteral(bg::i64{8})));
    QueueState::reset();
}
