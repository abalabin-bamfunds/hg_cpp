#pragma once

#include <hg/scalar/opaque.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/web/types.hpp>

namespace hg::web {
class ClientRuntime;
}

namespace hg::web::details {

template <template <typename, auto...> typename Field> struct ClientOutput {
    Field<TSD<i64, TS<Bundle<HttpResponse>>>> replies;
    Field<TS<Opaque<ClientRuntime *>>> runtime;
};

} // namespace hg::web::details
