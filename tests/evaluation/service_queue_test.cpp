#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

TEST_CASE("service requests dispatch to a worker and queued replies return to their clients",
          "[evaluation][feature:service][feature:realtime][series:tsd][transition:update][lifecycle][queue][concurrency]") {
    using namespace bg::literals;
    using namespace bg::util::literals;

    struct ServiceQueueState {
        struct Work {
            bg::service_client_id_t client_id;
            bg::i64 value;
        };

        struct Reply {
            char client;
            bg::i64 value;
        };

        ~ServiceQueueState() { stop_worker(); }

        void set_request_queue(bg::Queue value) {
            const auto first_key = bg::i64{101};
            const auto first_value = bg::i64{3};
            const auto second_key = bg::i64{202};
            const auto second_value = bg::i64{5};
            const auto accepted = value.write(
                ::literal::TimeSeriesLiteral{{first_key, first_value}, {second_key, second_value}});
            {
                const auto lock = std::scoped_lock{mutex};
                request_queue = std::move(value);
                initial_writes_accepted = accepted;
            }
            changed.notify_all();
        }

        void start_worker(bg::Queue reply_queue, std::stop_source stop_source) {
            worker = std::jthread{[this, reply_queue = std::move(reply_queue), stop_source](std::stop_token) mutable {
                run_worker(std::move(reply_queue), stop_source);
            }};
        }

        void stop_worker() noexcept {
            {
                const auto lock = std::scoped_lock{mutex};
                stopping = true;
            }
            changed.notify_all();
            if (worker.joinable()) {
                worker.join();
            }
        }

        void submit(const bg::In<bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>> &requests,
                    const bg::Scalar<bg::service_client_id_t> &client_id) {
            const auto request = requests[client_id];
            if (!request.valid()) {
                return;
            }

            {
                const auto lock = std::scoped_lock{mutex};
                evaluator_thread = std::this_thread::get_id();
                work.push_back({*client_id, *request});
                ++submitted;
            }
            changed.notify_all();
        }

        std::size_t record_reply(char client, bg::i64 value) {
            auto reply_count = std::size_t{};
            {
                const auto lock = std::scoped_lock{mutex};
                replies.push_back({client, value});
                reply_count = replies.size();
            }
            changed.notify_all();
            return reply_count;
        }

        void run_worker(bg::Queue reply_queue, std::stop_source stop_source) noexcept {
            {
                const auto lock = std::scoped_lock{mutex};
                worker_thread = std::this_thread::get_id();
            }

            while (true) {
                auto item = Work{};
                {
                    auto lock = std::unique_lock{mutex};
                    changed.wait(lock, [&] { return stopping || !work.empty(); });
                    if (stopping) {
                        return;
                    }
                    item = work.front();
                    work.pop_front();
                }

                const auto client_id = item.client_id;
                const auto result = item.value * 2;
                try {
                    if (!reply_queue.write(::literal::TimeSeriesLiteral{{client_id, result}})) {
                        throw std::runtime_error{"reply queue closed before the worker wrote its result"};
                    }
                } catch (...) {
                    {
                        const auto lock = std::scoped_lock{mutex};
                        worker_failure = std::current_exception();
                    }
                    stop_source.request_stop();
                    return;
                }

                {
                    const auto lock = std::scoped_lock{mutex};
                    processed.push_back(item);
                    ++completed;
                }
            }
        }

        std::mutex mutex;
        std::condition_variable changed;
        bg::Queue request_queue;
        std::jthread worker;
        std::deque<Work> work;
        std::vector<Work> processed;
        std::vector<Reply> replies;
        std::thread::id evaluator_thread;
        std::thread::id worker_thread;
        std::exception_ptr worker_failure;
        std::size_t submitted{};
        std::size_t completed{};
        bool initial_writes_accepted{};
        bool stopping{};
        bool update_write_accepted{};
        bool watchdog_timed_out{};
        bool finished{};
    };

    auto state = ServiceQueueState{};
    const auto service = bg::service<auto(bg::TS<bg::i64>)->bg::TS<bg::i64>>{"background_double", {"value"}};
    auto catalogue = bg::ServiceCatalogue{};
    auto stop_source = std::stop_source{};

    const auto requests = bg::queue<bg::TSD<bg::i64, bg::TS<bg::i64>>>(
        bg::start = [&state](bg::Queue queue) { state.set_request_queue(std::move(queue)); });
    const auto replies = bg::queue<bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>>(
        bg::start = [&state, &stop_source](bg::Queue queue) {
            state.start_worker(std::move(queue), stop_source);
        },
        bg::stop = [&state]() noexcept { state.stop_worker(); });

    const auto dispatch = bg::node([&state](bg::In<bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>> values) {
        values.for_each_added([&](const auto &client_id) { state.submit(values, client_id); });
        values.for_each_modified([&](const auto &client_id) { state.submit(values, client_id); });
    });
    const auto implementation = bg::graph(
        "name"_n = "background_double_impl",
        [&](bg::wp<bg::TSD<bg::service_client_id_t, bg::TS<bg::i64>>, "value"_n> values) {
            dispatch(values);
            return replies();
        });
    catalogue.register_impl(service, implementation);

    auto update_sent = false;
    auto watchdog = std::jthread{[&] {
        auto lock = std::unique_lock{state.mutex};
        if (state.changed.wait_for(lock, std::chrono::seconds{5}, [&] { return state.finished; })) {
            return;
        }
        state.watchdog_timed_out = true;
        lock.unlock();
        stop_source.request_stop();
    }};

    auto graph_failure = std::exception_ptr{};
    try {
        bg::run_graph(
            [&] {
                const auto input = requests();
                const auto first = service(input[bg::i64{101}]).as<bg::TS<bg::i64>>();
                const auto second = service(input[bg::i64{202}]).as<bg::TS<bg::i64>>();
                (void)bg::node([&](bg::In<bg::TS<bg::i64>> value) {
                    if (!value.valid()) {
                        return;
                    }
                    const auto reply_count = state.record_reply('A', *value);
                    if (reply_count == 3) {
                        stop_source.request_stop();
                    }
                    if (!update_sent) {
                        update_sent = true;
                        auto request_queue = bg::Queue{};
                        {
                            const auto lock = std::scoped_lock{state.mutex};
                            request_queue = state.request_queue;
                        }
                        const auto client_key = bg::i64{101};
                        const auto request_value = bg::i64{7};
                        const auto accepted = request_queue.write(
                            ::literal::TimeSeriesLiteral{{client_key, request_value}});
                        {
                            const auto lock = std::scoped_lock{state.mutex};
                            state.update_write_accepted = accepted;
                        }
                        state.changed.notify_all();
                        if (!accepted) {
                            stop_source.request_stop();
                        }
                    }
                })(first);
                (void)bg::node([&](bg::In<bg::TS<bg::i64>> value) {
                    if (!value.valid()) {
                        return;
                    }
                    const auto reply_count = state.record_reply('B', *value);
                    if (reply_count == 3) {
                        stop_source.request_stop();
                    }
                })(second);
            },
            bg::GraphRunOptions{.services = &catalogue,
                                .engine = bg::EvaluationEngine::realtime,
                                .stop_token = stop_source.get_token()});
    } catch (...) {
        graph_failure = std::current_exception();
    }

    {
        const auto lock = std::scoped_lock{state.mutex};
        state.finished = true;
    }
    state.changed.notify_all();
    watchdog.join();

    if (graph_failure != nullptr) {
        std::rethrow_exception(graph_failure);
    }
    if (state.worker_failure != nullptr) {
        std::rethrow_exception(state.worker_failure);
    }
    CHECK_FALSE(state.watchdog_timed_out);
    CHECK(state.initial_writes_accepted);
    CHECK(state.update_write_accepted);
    CHECK(state.submitted == 3);
    CHECK(state.completed == 3);
    CHECK_FALSE(state.worker.joinable());
    CHECK(state.evaluator_thread != std::thread::id{});
    CHECK(state.worker_thread != std::thread::id{});
    CHECK(state.worker_thread != state.evaluator_thread);

    auto actual = std::map<char, std::vector<bg::i64>>{};
    for (const auto &reply : state.replies) {
        actual[reply.client].push_back(reply.value);
    }
    CHECK(actual == std::map<char, std::vector<bg::i64>>{{'A', {6, 14}}, {'B', {10}}});

    REQUIRE(state.processed.size() == 3);
    const auto first_request = std::ranges::find(state.processed, bg::i64{3}, &ServiceQueueState::Work::value);
    const auto updated_request = std::ranges::find(state.processed, bg::i64{7}, &ServiceQueueState::Work::value);
    const auto other_request = std::ranges::find(state.processed, bg::i64{5}, &ServiceQueueState::Work::value);
    REQUIRE(first_request != state.processed.end());
    REQUIRE(updated_request != state.processed.end());
    REQUIRE(other_request != state.processed.end());
    CHECK(first_request->client_id == updated_request->client_id);
    CHECK(first_request->client_id != other_request->client_id);
}
