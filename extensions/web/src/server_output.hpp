#pragma once

#include <hg/scalar/opaque.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/web/types.hpp>

namespace hg::web {
class ServerRuntime;
namespace details {

template <template <typename, auto...> typename Field> struct ServerOutput {
    Field<TSD<i64, TSD<i64, TS<Bundle<HttpServerRequest>>>>> requests;
    Field<TSD<i64, TSD<i64, TS<Bundle<HttpServerRequest>>>>> websocket_requests;
    Field<TS<Opaque<ServerRuntime *>>> runtime;
};

} // namespace details
} // namespace hg::web
