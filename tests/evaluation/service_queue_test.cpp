#include "service_test_helpers.hpp"

#include <bg/queue_node.hpp>
#include <bg/run_graph.hpp>

#include <chrono>
#include <stop_token>
#include <thread>
#include <utility>

using namespace bg;

TEST_CASE("service requests dispatch to a worker and queued replies reach the client",
          "[evaluation][feature:service][feature:realtime][queue][concurrency]") {
    const auto service_definition = service<auto(TS<i64>)->TS<i64>>{"double", {"value"}};
    auto replies = Queue{};
    auto write_accepted = false;
    auto worker = std::jthread{};
    auto stop_source = std::stop_source{};
    auto actual = i64{};

    const auto response_queue = queue<TSD<service_client_id_t, TS<i64>>>(
        start = [&](Queue queue) { replies = std::move(queue); },
        stop = [&]() noexcept { if (worker.joinable()) worker.join(); });

    const auto dispatch = node([&](In<TSD<service_client_id_t, TS<i64>>> requests) {
        requests.for_each_added([&](const auto &client_id) {
            worker = std::jthread{[&, id = *client_id, value = *requests[client_id], queue = replies]() mutable {
                write_accepted = queue.write(::literal::TimeSeriesLiteral{{id, value * 2}});
            }};
        });
    });

    const auto implementation = graph(
        "name"_n = "double_impl", [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> requests) {
            dispatch(requests);
            return response_queue();
        });
    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(service_definition, implementation);

    run_graph(
        [&] {
            const auto output = service_definition(operators::const_(i64{3})).as<TS<i64>>();
            (void)node([&](i64 value) {
                actual = value;
                stop_source.request_stop();
            })(output);
        },
        GraphRunOptions{
            .services = &catalogue,
            .engine = EvaluationEngine::realtime,
            .run_time = std::chrono::seconds{5},
            .stop_token = stop_source.get_token(),
        });

    CHECK(actual == 6);
    CHECK(write_accepted);
    CHECK_FALSE(worker.joinable());
}
