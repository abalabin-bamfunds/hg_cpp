#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/web/types.hpp>

namespace bg::web {
class WebSocketClientRuntime;
namespace details {

template <template <typename, auto...> typename Field> struct WebSocketClientOutput {
    Field<TSD<i64, TSD<i64, TS<Bundle<HttpResponse>>>>> connect_responses;
    Field<TSD<i64, TSD<i64, TS<Bundle<WebSocketClose>>>>> closes;
    Field<TS<string>> errors;
    Field<TS<Opaque<WebSocketClientRuntime *>>> runtime;
};

} // namespace details
} // namespace bg::web
