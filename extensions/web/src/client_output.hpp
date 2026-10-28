#pragma once

#include <bg/scalar/opaque.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/web/types.hpp>

namespace bg::web {
class ClientRuntime;
}

namespace bg::web::details {

template <template <typename, auto...> typename Field> struct ClientReply {
    Field<i64> client_sequence;
    Field<Bundle<HttpResponse>> response;
};

template <template <typename, auto...> typename Field> struct ClientOutput {
    Field<TSD<i64, TS<Bundle<ClientReply>>>> replies;
    Field<TS<string>> errors;
    Field<TS<Opaque<ClientRuntime *>>> runtime;
};

} // namespace bg::web::details
