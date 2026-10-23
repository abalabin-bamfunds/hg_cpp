#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_client.hpp>

#include "client_output.hpp"
#include "client_runtime.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::web {
namespace {

using namespace bg::literals;
struct ClientSource {
    Scalar<Opaque<HttpClientOptions>> options;
    State<ClientRuntime *> runtime_state;
    Out<TSB<details::ClientOutput>> output;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        auto output_queue = context->runtime_graph().make_queue(context->node_index());
        auto runtime = std::make_unique<ClientRuntime>(options.value(), std::move(output_queue));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }
    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

struct ClientRequestSink {
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> client_id;
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> generation;
    In<TS<Bundle<HttpClientRequest>>> request;
    In<TS<Opaque<ClientRuntime *>>, passive> runtime_input;
    State<bool> submitted;

    void start() const { *submitted = false; }

    void evaluate() const {
        runtime_input.value()->request(client_id.value(), generation.value(),
                                       ScalarValue<Bundle<HttpClientRequest>>{request.value()});
        *submitted = true;
    }

    void stop() const noexcept {
        if (*submitted) runtime_input.value()->cancel(client_id.value(), generation.value());
    }
};

const auto client_source = node<ClientSource>();
const auto client_request_sink = node<ClientRequestSink>();
const auto error_sink = node([](In<TS<string>> error) { throw std::runtime_error{std::string{error.value()}}; });

} // namespace

std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options) {
    auto implementation = graph(
        "name"_n = "http_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>, "request"_n> requests)
            -> wp<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>> {
            const auto source = client_source(ScalarValue<Opaque<HttpClientOptions>>{options});

            const auto client = graph(
                "name"_n = "http_client_request",
                [](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TS<Bundle<HttpClientRequest>>, "request"_n> request,
                   wp<TSD<service_client_id_t, TS<Bundle<details::ClientReply>>>, "replies"_n> replies,
                   wp<TS<Opaque<ClientRuntime *>>, "runtime"_n> runtime) -> wp<TS<Bundle<HttpResponse>>> {
                    const auto generation = service_client_id();
                    const auto reply = operators::getitem(replies, client_id);
                    const auto is_current = operators::eq(operators::getattr(reply, util::symbol{"generation"}),
                                                          generation);
                    client_request_sink(client_id, generation, request, runtime);
                    return wp<TS<Bundle<HttpResponse>>>{
                        operators::filter_(is_current, operators::getattr(reply, util::symbol{"response"}))};
                });

            const auto response = map(client, requests, pass_through(source->replies), source->runtime);
            error_sink(source->errors);
            return wp<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>>{response};
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

} // namespace bg::web
