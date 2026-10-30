#pragma once

#include <bg/node.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include "client_output.hpp"
#include "client_runtime.hpp"

#include <memory>
#include <utility>

namespace bg::web::details {

struct ClientSource {
    Scalar<Opaque<HttpClientOptions>> options;
    State<ClientRuntime *> runtime_state;
    Out<TSB<ClientOutput>> output;

    static constexpr bool deduplicate_wiring = false;

    void start(Queue output_queue) const {
        auto runtime = std::make_unique<ClientRuntime>(options.value(), std::move(output_queue));
        output->runtime.set(runtime.get());
        *runtime_state = runtime.release();
    }

    void stop() const noexcept { delete std::exchange(*runtime_state, nullptr); }
    void finalize() const noexcept { delete std::exchange(*runtime_state, nullptr); }
};

inline const auto client_source = node<ClientSource>();

} // namespace bg::web::details
