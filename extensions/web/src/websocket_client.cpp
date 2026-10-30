#include <bg/graph.hpp>
#include <bg/library.hpp>
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
#include "websocket_client_source_replies.hpp"
#include "websocket_client_runtime.hpp"
#include "websocket_client_update_sink.hpp"
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

    static constexpr bool deduplicate_wiring = false;

    void start(Queue output_queue) const {
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

    static constexpr bool deduplicate_wiring = false;
    static constexpr auto queue_mode = QueueMode::batch;

    void start(Queue messages_queue) const {
        runtime.value()->attach_message_queue(std::move(messages_queue));
    }
};

const auto websocket_client_source = node<WebSocketClientSource>();
const auto websocket_client_messages_source = node<WebSocketClientMessagesSource>();
} // namespace

std::shared_ptr<const WiringTemplate> websocket_client_impl(WebSocketClientOptions options) {
    auto implementation = graph(
        "name"_n = "websocket_client_impl",
        [options = std::move(options)](
            wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientRequest>>, "request"_n> requests)
            -> wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>> {
            const auto source = details::wire_websocket_client_source_replies(options);

            const auto request_sink = graph(
                "name"_n = "websocket_client_request_sink",
                [options](wp<TS<service_client_id_t>, "key"_n> client_id,
                   wp<TSB<BinaryWebSocketClientRequest>, "request"_n> request,
                   wp<TS<Opaque<WebSocketClientRuntime *>>, "runtime"_n> runtime) {
                    const auto connect = request->connect_request;
                    details::websocket_client_update_sink(
                        client_id, connect, request->message, request->close, connect,
                        operators::nothing[type<TS<string>>](), runtime,
                        operators::nothing[type<TS<Bundle<Auth>>>](), options);
                });

            map(request_sink, requests, source->runtime);
            return wp<TSD<service_client_id_t, TSB<BinaryWebSocketClientReply>>>{source->replies};
        });
    return std::make_shared<decltype(implementation)>(std::move(implementation));
}

wp<TSB<details::WebSocketClientSourceReplies>>
details::wire_websocket_client_source_replies(WebSocketClientOptions options) {
    const auto source = websocket_client_source(std::move(options));
    const auto messages = websocket_client_messages_source(source->runtime);

    const auto replies = graph(
        "name"_n = "websocket_client_replies",
        [](wp<TS<service_client_id_t>, "key"_n> client_id,
           wp<TS<i64>, "sequence"_n> sequence,
           wp<TSD<i64, TSD<i64, TS<Bundle<HttpResponse>>>>, "connect_responses"_n> connect_responses,
           wp<TSD<i64, TSD<i64, TS<List<bytes>>>>, "messages"_n> messages,
           wp<TSD<i64, TSD<i64, TS<Bundle<WebSocketClose>>>>, "closes"_n> closes)
            -> wp<TSB<BinaryWebSocketClientReply>> {
            const auto current = [&](auto dictionary) {
                const auto values = dictionary[client_id];
                return values[sequence];
            };
            return bundle<BinaryWebSocketClientReply>(
                "connect_response"_n = current(connect_responses), "messages"_n = current(messages),
                "close"_n = current(closes));
        });

    const auto reply_values = map(replies, source->current_sequences,
                                  pass_through(source->connect_responses), pass_through(messages),
                                  pass_through(source->closes));
    return bundle<details::WebSocketClientSourceReplies>("runtime"_n = source->runtime,
                                                          "replies"_n = reply_values);
}

} // namespace bg::web
