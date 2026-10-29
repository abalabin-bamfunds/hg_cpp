#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/queue_node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/websocket_client.hpp>

#include "websocket_client_output.hpp"
#include "websocket_client_runtime.hpp"
#include "runtime_logging.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace bg::web {
namespace {

using namespace bg::literals;

struct WebSocketClientSource {
    Scalar<Opaque<WebSocketClientOptions>> options;
    State<WebSocketClientRuntime *> runtime_state;
    Out<TSB<details::WebSocketClientOutput>> output;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        auto output_queue = context->runtime_graph().make_queue(context->node_index());
        auto runtime = std::make_unique<WebSocketClientRuntime>(options.value(), std::move(output_queue));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }
    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

struct WebSocketClientMessagesSource {
    In<TS<Opaque<WebSocketClientRuntime *>>, passive> runtime;
    Out<TSD<i64, TSD<i64, TS<List<bytes>>>>> messages;

    static constexpr bool uses_queue = true;
    static constexpr bool deduplicate_wiring = false;
    static constexpr auto queue_mode = QueueMode::batch;

    void start(EvaluationContext *context) const {
        runtime.value()->attach_message_queue(context->runtime_graph().make_queue(context->node_index()));
    }
};

struct WebSocketClientUpdateSink {
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> client_id;
    In<TS<Bundle<WebSocketConnectRequest>>, optional> connect;
    In<TS<bytes>, optional> message;
    In<TS<Bundle<WebSocketClose>>, optional> close;
    In<TS<Opaque<WebSocketClientRuntime *>>, passive> runtime;
    State<i64> sequence;

    void start() const {
        *sequence = 0;
    }

    void evaluate() const {
        try {
            auto connect_value = std::optional<ScalarValue<Bundle<WebSocketConnectRequest>>>{};
            auto sequence_value = *sequence;
            if (connect.modified() && connect.valid()) {
                auto *client_runtime = runtime.value();
                sequence_value = client_runtime->next_connection_sequence();
                connect_value.emplace(connect.value());
            }

            auto message_value = std::optional<ScalarValue<bytes>>{};
            if (message.modified() && message.valid()) message_value.emplace(message.value());
            auto close_value = std::optional<ScalarValue<Bundle<WebSocketClose>>>{};
            if (close.modified() && close.valid()) close_value.emplace(close.value());
            if (!connect_value && !message_value && !close_value) return;

            runtime.value()->update(client_id.value(), sequence_value, std::move(connect_value), std::move(message_value),
                                    std::move(close_value));
            *sequence = sequence_value;
        } catch (...) {
            details::log_runtime_failure("WebSocket client update submission failed", std::current_exception());
        }
    }

    void stop() const noexcept {
        if (*sequence != 0) runtime.value()->cancel(client_id.value(), *sequence);
    }
};

const auto websocket_client_source = node<WebSocketClientSource>();
const auto websocket_client_messages_source = node<WebSocketClientMessagesSource>();
const auto websocket_client_update_sink = node<WebSocketClientUpdateSink>();
} // namespace

std::shared_ptr<const WiringTemplate> websocket_client_impl(WebSocketClientOptions options) {
    auto implementation = graph(
        "name"_n = "websocket_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientRequest>>, "request"_n> requests)
            -> wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>> {
            const auto source = websocket_client_source(options);
            const auto messages = websocket_client_messages_source(source->runtime);

            const auto request_sink = graph(
                "name"_n = "websocket_client_request_sink",
                [](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TSB<BinaryWebSocketClientRequest>, "request"_n> request,
                   wp<TS<Opaque<WebSocketClientRuntime *>>, "runtime"_n> runtime) {
                    websocket_client_update_sink(
                        client_id, operators::getattr(request, util::symbol{"connect_request"}),
                        operators::getattr(request, util::symbol{"message"}),
                        operators::getattr(request, util::symbol{"close"}), runtime);
                });

            const auto replies = graph(
                "name"_n = "websocket_client_replies",
                [](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TS<i64>, "sequence"_n> sequence,
                   wp<TSD<i64, TSD<i64, TS<Bundle<HttpResponse>>>>, "connect_responses"_n> connect_responses,
                   wp<TSD<i64, TSD<i64, TS<List<bytes>>>>, "messages"_n> messages,
                   wp<TSD<i64, TSD<i64, TS<Bundle<WebSocketClose>>>>, "closes"_n> closes)
                    -> wp<TSB<BinaryWebSocketClientReply>> {
                    const auto current = [&](auto dictionary) {
                        const auto values = operators::getitem(dictionary, client_id);
                        return operators::getitem(values, sequence);
                    };
                    const auto response = current(connect_responses);
                    const auto received_messages = current(messages);
                    const auto close_value = current(closes);
                    return bundle<BinaryWebSocketClientReply>(
                        "connect_response"_n = response,
                        "messages"_n = received_messages,
                        "close"_n = close_value);
                });

            map(request_sink, requests, source->runtime);
            const auto reply_values = map(replies, source->current_sequences,
                                          pass_through(source->connect_responses), pass_through(messages),
                                          pass_through(source->closes));
            return wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>>{reply_values};
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

} // namespace bg::web
