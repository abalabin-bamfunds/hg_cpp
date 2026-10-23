#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/type/opaque_scalar_type.hpp>
#include <bg/web/types.hpp>

namespace bg::web {
class ServerRuntime;
namespace details {

template <template <typename, auto...> typename Field> struct ServerOutput {
    Field<TS<i64>> port;
    Field<TSD<i64, TS<Bundle<HttpServerRequest>>>> requests;
    Field<TS<string>> errors;
    Field<TS<ServerRuntime *>> runtime;
};

} // namespace details
} // namespace bg::web

namespace bg {

template <> struct scalar_type_traits<web::ServerRuntime *> {
    static const ScalarType &type() noexcept { return cpp_scalar_type<Opaque<web::ServerRuntime *>>(); }
};

template <> struct scalar_storage_traits<web::ServerRuntime *>
    : scalar_storage_traits<Opaque<web::ServerRuntime *>> {};

} // namespace bg
