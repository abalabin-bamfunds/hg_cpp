#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/websocket_client.hpp>

#include "runtime_logging.hpp"
#include "websocket_client_runtime.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace bg::web::details {

struct WebSocketClientUpdateSink {
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> client_id;
    In<TS<Bundle<WebSocketConnectRequest>>, optional> connect;
    In<TS<bytes>, optional> message;
    In<TS<Bundle<WebSocketClose>>, optional> close;
    In<TS<Bundle<WebSocketConnectRequest>>, optional> ready_connect;
    In<TS<string>, optional> error;
    In<TS<Opaque<WebSocketClientRuntime *>>, passive> runtime;
    In<TS<Bundle<Auth>>, optional, passive> credentials;
    Scalar<Opaque<WebSocketClientOptions>> options;
    State<i64> sequence;
    State<bool> waiting;
    State<bool> submitted;
    State<std::chrono::steady_clock::time_point> deadline;
    State<ScheduleHandle> deadline_wake;

    static constexpr bool uses_timed_scheduling = true;

    void start() const {
        *sequence = 0;
        *waiting = false;
        *submitted = false;
        *deadline = {};
        *deadline_wake = {};
    }

    void evaluate(EvaluationContext *context) const {
        auto *client = runtime.value();
        const auto id = client_id.value();

        if (connect.modified()) {
            cancel_deadline(*context);
            if (!connect.valid()) {
                if (*submitted) client->cancel(id, *sequence);
                *waiting = false;
                *submitted = false;
            } else {
                auto next_sequence = i64{};
                try {
                    next_sequence = client->next_connection_sequence();
                    client->begin_connect(id, next_sequence);
                    *sequence = next_sequence;
                    *waiting = true;
                    *submitted = false;
                    *deadline = std::chrono::steady_clock::now() + options.value().handshake_timeout;
                } catch (...) {
                    log_runtime_failure("WebSocket client update submission failed", std::current_exception());
                    if (next_sequence != 0) {
                        client->fail(id, next_sequence, "WebSocket connection submission failed");
                    }
                    *waiting = false;
                    *submitted = false;
                }
            }
        }

        if (close.modified() && close.valid()) {
            if (*sequence != 0) {
                if (*submitted) {
                    try {
                        auto message_value = std::optional<ScalarValue<bytes>>{};
                        if (message.modified() && message.valid()) message_value.emplace(message.value());
                        client->update(id, *sequence, std::nullopt, std::move(message_value),
                                       ScalarValue<Bundle<WebSocketClose>>{close.value()}, std::nullopt, *deadline);
                    } catch (...) {
                        log_runtime_failure("WebSocket client update submission failed", std::current_exception());
                    }
                } else {
                    client->fail(id, *sequence, "WebSocket handshake canceled by close request");
                }
            }
            *waiting = false;
            *submitted = false;
            cancel_deadline(*context);
            return;
        }

        if (*waiting && std::chrono::steady_clock::now() >= *deadline) {
            client->fail(id, *sequence, "WebSocket connection timed out waiting for credentials");
            *waiting = false;
            cancel_deadline(*context);
            return;
        }

        if (*waiting && ready_connect.modified() && ready_connect.valid()) {
            const auto failure = error.valid() ? std::string{error.value()} : std::string{};
            if (!failure.empty()) {
                client->fail(id, *sequence, failure);
            } else {
                try {
                    auto auth = std::optional<ScalarValue<Bundle<Auth>>>{};
                    if (credentials.valid()) auth.emplace(credentials.value());
                    auto message_value = std::optional<ScalarValue<bytes>>{};
                    if (message.modified() && message.valid()) message_value.emplace(message.value());
                    client->update(id, *sequence, ScalarValue<Bundle<WebSocketConnectRequest>>{ready_connect.value()},
                                   std::move(message_value), std::nullopt, std::move(auth), *deadline);
                    *submitted = true;
                } catch (...) {
                    log_runtime_failure("WebSocket client update submission failed", std::current_exception());
                }
            }
            *waiting = false;
            cancel_deadline(*context);
            return;
        }

        if (*waiting) {
            if (!*deadline_wake) {
                const auto remaining = *deadline - std::chrono::steady_clock::now();
                *deadline_wake = context->schedule_wall_clock_at(
                    context->wall_clock().now() + std::chrono::ceil<std::chrono::microseconds>(remaining));
            }
            return;
        }

        if (!*submitted) return;
        try {
            auto message_value = std::optional<ScalarValue<bytes>>{};
            if (message.modified() && message.valid()) message_value.emplace(message.value());
            if (!message_value) return;
            client->update(id, *sequence, std::nullopt, std::move(message_value), std::nullopt, std::nullopt,
                           *deadline);
        } catch (...) {
            log_runtime_failure("WebSocket client update submission failed", std::current_exception());
        }
    }

    void stop(EvaluationContext *context) const noexcept {
        cancel_deadline(*context);
        if (*sequence != 0) runtime.value()->cancel(client_id.value(), *sequence);
    }

  private:
    void cancel_deadline(EvaluationContext &context) const noexcept {
        if (*deadline_wake) context.cancel(*deadline_wake);
        *deadline_wake = {};
    }
};

inline const auto websocket_client_update_sink = node<WebSocketClientUpdateSink>();

} // namespace bg::web::details
