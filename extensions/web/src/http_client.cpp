#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/web/http_client.hpp>

#include "client_output.hpp"
#include "client_runtime.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
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

struct ClientDispatchState {
    service_client_id_t next_generation{1};
    std::unordered_map<service_client_id_t, service_client_id_t> active_generations;
};

struct ClientDispatch {
    In<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>, optional> requests;
    In<TSD<service_client_id_t, TS<Bundle<details::ClientReply>>>, optional> replies;
    In<TS<Opaque<ClientRuntime *>>, passive> runtime_input;
    State<ClientDispatchState *> state;
    Out<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>> output;

    static constexpr bool deduplicate_wiring = false;

    void start() const { *state = new ClientDispatchState{}; }

    void evaluate() const {
        auto &dispatch_state = **state;
        auto *runtime = runtime_input.value();

        if (requests.modified()) {
            requests.for_each_removed([&](const auto &client_id) {
                const auto found = dispatch_state.active_generations.find(client_id.value());
                if (found == dispatch_state.active_generations.end()) return;
                runtime->cancel(client_id.value(), found->second);
                dispatch_state.active_generations.erase(found);
                (void)output.erase(client_id);
            });

            if (requests.valid()) {
                const auto dispatch = [&](const auto &client_id) {
                    const auto request = requests[client_id];
                    if (!request.valid()) return;
                    const auto id = client_id.value();
                    auto found = dispatch_state.active_generations.find(id);
                    if (found == dispatch_state.active_generations.end()) {
                        if (dispatch_state.next_generation == std::numeric_limits<service_client_id_t>::max()) {
                            throw std::overflow_error{"HTTP client activation generation exhausted"};
                        }
                        found = dispatch_state.active_generations.emplace(id, dispatch_state.next_generation++).first;
                    }
                    runtime->request(id, found->second, ScalarValue<Bundle<HttpClientRequest>>{request.value()});
                };
                requests.for_each_added(dispatch);
                requests.for_each_modified(dispatch);
            }
        }

        const auto apply_reply = [&](const auto &client_id) {
            const auto reply = replies[client_id];
            if (!reply.valid()) return;
            const auto value = reply.value();
            const auto generation = scalar_cast<service_client_id_t>(value[0]).value();
            const auto found = dispatch_state.active_generations.find(client_id.value());
            if (found == dispatch_state.active_generations.end() || found->second != generation) return;
            output[client_id].set_value(scalar_cast<Bundle<HttpResponse>>(value[1]));
        };
        if (replies.valid() && replies.modified()) {
            replies.for_each_added(apply_reply);
            replies.for_each_modified(apply_reply);
        }
    }

    void stop() const noexcept { delete std::exchange(*state, nullptr); }
    void finalize() const noexcept { delete std::exchange(*state, nullptr); }
};

const auto client_source = node<ClientSource>();
const auto client_dispatch = node<ClientDispatch>();
const auto error_sink = node([](In<TS<string>> error) { throw std::runtime_error{std::string{error.value()}}; });

} // namespace

std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options) {
    auto implementation = graph(
        "name"_n = "http_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>, "request"_n> requests) {
            const auto source = client_source(ScalarValue<Opaque<HttpClientOptions>>{options});
            const auto response = client_dispatch(requests, source->replies, source->runtime);
            error_sink(source->errors);
            return response;
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

} // namespace bg::web
