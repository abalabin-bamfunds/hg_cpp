#include <bg/graph.hpp>
#include <bg/node.hpp>
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
class ClientSource {
  public:
    using compiled_data_type = HttpClientOptions;
    using state_type = ClientRuntime *;
    using Invocation = NodeInvocation<compiled_data_type, state_type>;
    using output_time_series_type = TSB<details::ClientOutput>;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    explicit ClientSource(HttpClientOptions options)
        : options_{std::move(options)},
          signature_{util::symbol{"http_client_source"}, {}, TSB<details::ClientOutput>::type()} {}

    const Signature &signature() const noexcept { return signature_; }
    HttpClientOptions compile(const ResolvedSignature &, allocator &) const { return options_; }

    static void start(Invocation invocation) {
        auto output_queue = invocation.context().runtime_graph().make_queue(invocation.context().node_index());
        auto runtime = std::make_unique<ClientRuntime>(invocation.compiled_data(), std::move(output_queue));
        auto output = invocation.output_as<Out<TSB<details::ClientOutput>>>();
        output->runtime.set(runtime.get());
        invocation.state() = runtime.release();
    }

    static void stop(Invocation invocation) noexcept { delete std::exchange(invocation.state(), nullptr); }
    static void destroy_state(Invocation invocation) noexcept { delete std::exchange(invocation.state(), nullptr); }

  private:
    HttpClientOptions options_;
    Signature signature_;
};

struct ClientDispatchState {
    service_client_id_t next_generation{1};
    std::unordered_map<service_client_id_t, service_client_id_t> active_generations;
};

class ClientDispatch {
  public:
    using compiled_data_type = void;
    using state_type = ClientDispatchState *;
    using Invocation = NodeInvocation<compiled_data_type, state_type>;
    using output_time_series_type = TSD<service_client_id_t, TS<Bundle<HttpResponse>>>;

    ClientDispatch()
        : signature_{util::symbol{"http_client_dispatch"},
                     {{.name = util::symbol{"requests"},
                       .type = &TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>::type(),
                       .validity = optional},
                      {.name = util::symbol{"replies"},
                       .type = &TSD<service_client_id_t, TS<Bundle<details::ClientReply>>>::type(),
                       .validity = optional},
                      {.name = util::symbol{"runtime"}, .type = &TS<Opaque<ClientRuntime *>>::type()}},
                     TSD<service_client_id_t, TS<Bundle<HttpResponse>>>::type()} {}

    const Signature &signature() const noexcept { return signature_; }

    static void start(Invocation invocation) { invocation.state() = new ClientDispatchState{}; }

    static void evaluate(Invocation invocation) {
        auto &state = *invocation.state();
        const auto requests = invocation.input<In<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>>>(0);
        const auto replies = invocation.input<In<TSD<service_client_id_t, TS<Bundle<details::ClientReply>>>>>(1);
        const auto runtime_input = invocation.input<In<TS<Opaque<ClientRuntime *>>>>(2);
        if (!runtime_input.valid()) return;
        auto *runtime = runtime_input.value();
        auto output = invocation.output_as<Out<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>>>();

        if (requests.valid() && requests.modified()) {
            requests.for_each_removed([&](const auto &client_id) {
                const auto found = state.active_generations.find(client_id.value());
                if (found == state.active_generations.end()) return;
                runtime->cancel(client_id.value(), found->second);
                state.active_generations.erase(found);
                (void)output.erase(client_id);
            });

            const auto dispatch = [&](const auto &client_id) {
                const auto request = requests[client_id];
                if (!request.valid()) return;
                const auto id = client_id.value();
                auto found = state.active_generations.find(id);
                if (found == state.active_generations.end()) {
                    if (state.next_generation == std::numeric_limits<service_client_id_t>::max()) {
                        throw std::overflow_error{"HTTP client activation generation exhausted"};
                    }
                    found = state.active_generations.emplace(id, state.next_generation++).first;
                }
                runtime->request(id, found->second, ScalarValue<Bundle<HttpClientRequest>>{request.value()});
            };
            requests.for_each_added(dispatch);
            requests.for_each_modified(dispatch);
        }

        const auto apply_reply = [&](const auto &client_id) {
            const auto reply = replies[client_id];
            if (!reply.valid()) return;
            const auto value = reply.value();
            const auto generation = scalar_cast<service_client_id_t>(value[0]).value();
            const auto found = state.active_generations.find(client_id.value());
            if (found == state.active_generations.end() || found->second != generation) return;
            output[client_id].set_value(scalar_cast<Bundle<HttpResponse>>(value[1]));
        };
        if (replies.valid() && replies.modified()) {
            replies.for_each_added(apply_reply);
            replies.for_each_modified(apply_reply);
        }
    }

    static void stop(Invocation invocation) noexcept { delete std::exchange(invocation.state(), nullptr); }
    static void destroy_state(Invocation invocation) noexcept { delete std::exchange(invocation.state(), nullptr); }

  private:
    Signature signature_;
};

} // namespace

std::shared_ptr<const WiringTemplate> http_client_impl(HttpClientOptions options) {
    auto implementation = graph(
        "name"_n = "http_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TS<Bundle<HttpClientRequest>>>, "request"_n> requests) {
            auto source_output = node(ClientSource{options})();
            if (!source_output) throw std::logic_error{"HTTP client source must produce output"};
            const auto source = wp<TSB<details::ClientOutput>>{*source_output};
            const auto response = node(ClientDispatch{})(requests, source->replies, source->runtime);
            if (!response) throw std::logic_error{"HTTP client dispatcher must produce responses"};
            static const auto error_sink = node([](In<TS<string>> error) {
                if (error.valid() && error.modified()) throw std::runtime_error{std::string{error.value()}};
            });
            error_sink(source->errors);
            return wp<TSD<service_client_id_t, TS<Bundle<HttpResponse>>>>{*response};
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

} // namespace bg::web
