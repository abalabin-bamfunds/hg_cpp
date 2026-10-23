#pragma once

#include <bg/scalar/bytes.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/string.hpp>
#include <bg/scalar/tuple/type.hpp>

namespace bg::web {

template <template <typename, auto...> typename Field> struct HttpPeer {
    Field<string> address;
    Field<i64> port;
};

template <template <typename, auto...> typename Field> struct HttpServerRequest {
    Field<string> method;
    Field<string> target;
    Field<string> path;
    Field<List<Tuple<string, string>>> query;
    Field<List<Tuple<string, string>>> headers;
    Field<bytes> body;
    Field<Bundle<HttpPeer>> peer;
};

template <template <typename, auto...> typename Field> struct HttpResponse {
    Field<i64> status;
    Field<List<Tuple<string, string>>> headers;
    Field<bytes> body;
};

} // namespace bg::web
