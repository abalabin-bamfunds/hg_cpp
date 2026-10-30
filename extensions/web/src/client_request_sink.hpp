#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/web/http_client.hpp>

#include "client_runtime.hpp"
#include "runtime_logging.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

namespace bg::web::details {

using namespace bg::literals;

struct ClientRequestSink {
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> client_id;
    In<TS<service_client_id_t, TimeSeriesGuarantee::const_>, passive> client_sequence;
    In<TS<Bundle<HttpClientRequest>>, optional> request;
    In<TS<Bundle<HttpClientRequest>>, optional> ready_request;
    In<TS<string>, optional> request_error;
    In<TS<Opaque<ClientRuntime *>>, passive> runtime_input;
    In<TS<Bundle<Auth>>, optional, passive> credentials;
    Scalar<Opaque<HttpClientOptions>> options;
    State<bool> waiting;
    State<bool> submitted;
    State<std::chrono::steady_clock::time_point> deadline;
    State<ScheduleHandle> deadline_wake;

    static constexpr bool uses_timed_scheduling = true;

    void start() const {
        *waiting = false;
        *submitted = false;
        *deadline = {};
        *deadline_wake = {};
    }

    void evaluate(EvaluationContext *context) const {
        const auto id = client_id.value();
        const auto sequence = client_sequence.value();
        auto *client = runtime_input.value();

        if (request.modified()) {
            cancel_deadline(*context);
            if (!request.valid()) {
                *waiting = false;
                if (*submitted) client->cancel(id, sequence);
                *submitted = false;
            } else {
                *waiting = true;
                *deadline = std::chrono::steady_clock::now() + options.value().timeout;
            }
        }

        if (*waiting && std::chrono::steady_clock::now() >= *deadline) {
            client->fail(id, sequence, "HTTP request timed out waiting for credentials");
            *waiting = false;
            cancel_deadline(*context);
            return;
        }

        if (!*waiting) return;
        if (!ready_request.modified() || !ready_request.valid()) {
            if (!*deadline_wake) {
                const auto remaining = *deadline - std::chrono::steady_clock::now();
                *deadline_wake = context->schedule_wall_clock_at(
                    context->wall_clock().now() + std::chrono::ceil<std::chrono::microseconds>(remaining));
            }
            return;
        }

        const auto request_failure = request_error.valid() ? std::string{request_error.value()} : std::string{};
        if (!request_failure.empty()) {
            client->fail(id, sequence, request_failure);
        } else {
            try {
                auto auth = std::optional<ScalarValue<Bundle<Auth>>>{};
                if (credentials.valid()) auth.emplace(credentials.value());
                client->request(id, sequence, ScalarValue<Bundle<HttpClientRequest>>{ready_request.value()},
                                std::move(auth), *deadline);
                *submitted = true;
            } catch (...) {
                log_runtime_failure("HTTP client request submission failed", std::current_exception());
            }
        }

        *waiting = false;
        cancel_deadline(*context);
    }

    void stop(EvaluationContext *context) const noexcept {
        cancel_deadline(*context);
        if (*submitted) runtime_input.value()->cancel(client_id.value(), client_sequence.value());
    }

  private:
    void cancel_deadline(EvaluationContext &context) const noexcept {
        if (*deadline_wake) context.cancel(*deadline_wake);
        *deadline_wake = {};
    }
};

inline const auto client_request_sink = node<ClientRequestSink>();

} // namespace bg::web::details
