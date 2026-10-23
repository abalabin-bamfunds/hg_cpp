#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/web/types.hpp>

namespace bg::web {
class ServerRuntime;
namespace details {

template <template <typename, auto...> typename Field> struct ServerOutput {
    Field<TS<i64>> port;
    Field<TSD<i64, TS<Bundle<HttpServerRequest>>>> requests;
    Field<TS<string>> errors;
    Field<TS<Opaque<ServerRuntime *>>> runtime;
};

} // namespace details
} // namespace bg::web
