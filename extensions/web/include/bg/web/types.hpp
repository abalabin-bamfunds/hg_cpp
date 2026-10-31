#pragma once

#include <bg/scalar/bytes.hpp>
#include <bg/scalar/bundle/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/string.hpp>
#include <bg/scalar/tuple/type.hpp>
#include <bg/schema/reflection.hpp>
#include <bg/time_series/time_series_type.hpp>

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

template <template <typename, auto...> typename Field> struct HttpClientRequest {
    Field<string> method;
    Field<string> url;
    Field<List<Tuple<string, string>>> headers;
    Field<bytes> body;
};

template <template <typename, auto...> typename Field> struct WebSocketConnectRequest {
    Field<string> url;
    Field<List<Tuple<string, string>>> headers;
};

template <template <typename, auto...> typename Field> struct HttpResponse {
    Field<i64> status;
    Field<List<Tuple<string, string>>> headers;
    Field<bytes> body;
};

template <template <typename, auto...> typename Field> struct Auth {};

template <template <typename, auto...> typename Field> struct AnonymousAuth : schema_extends<Auth, Field> {};

template <template <typename, auto...> typename Field> struct BasicAuth : schema_extends<Auth, Field> {
    Field<string> username;
    Field<string> password;
};

template <template <typename, auto...> typename Field> struct DigestAuth : schema_extends<Auth, Field> {
    Field<string> username;
    Field<string> password;
};

template <template <typename, auto...> typename Field> struct NegotiateAuth : schema_extends<Auth, Field> {};

template <template <typename, auto...> typename Field> struct NtlmAuth : schema_extends<Auth, Field> {
    Field<string> username;
    Field<string> password;
};

template <template <typename, auto...> typename Field> struct BearerAuth : schema_extends<Auth, Field> {
    Field<string> token;
};

template <template <typename, auto...> typename Field> struct HeaderAuth : schema_extends<Auth, Field> {
    Field<List<Tuple<string, string>>> headers;
};

template <template <typename, auto...> typename Field> struct WebSocketClose {
    Field<i64> code;
    Field<string> reason;
};

template <typename T> struct WebSocketServerRequest {
    template <template <typename, auto...> typename Field> struct schema {
        Field<TS<Bundle<HttpServerRequest>>> connect_request;
        Field<TS<List<T>>> messages;
    };
};

template <typename T> struct WebSocketClientRequest {
    template <template <typename, auto...> typename Field> struct schema {
        Field<TS<Bundle<WebSocketConnectRequest>>> connect_request;
        Field<TS<T>> message;
        Field<TS<Bundle<WebSocketClose>>> close;
    };
};

template <typename T> struct WebSocketClientReply {
    template <template <typename, auto...> typename Field> struct schema {
        Field<TS<Bundle<HttpResponse>>> connect_response;
        Field<TS<List<T>>> messages;
        Field<TS<Bundle<WebSocketClose>>> close;
    };
};

template <typename T> struct WebSocketResponse {
    template <template <typename, auto...> typename Field> struct schema {
        Field<TS<Bundle<HttpResponse>>> connect_response;
        Field<TS<T>> message;
        Field<TS<Bundle<WebSocketClose>>> close;
    };
};

template <template <typename, auto...> typename Field>
using BinaryWebSocketClientRequest = WebSocketClientRequest<bytes>::template schema<Field>;

template <template <typename, auto...> typename Field>
using BinaryWebSocketClientReply = WebSocketClientReply<bytes>::template schema<Field>;

template <template <typename, auto...> typename Field>
using BinaryWebSocketServerRequest = WebSocketServerRequest<bytes>::template schema<Field>;

template <template <typename, auto...> typename Field>
using BinaryWebSocketResponse = WebSocketResponse<bytes>::template schema<Field>;

} // namespace bg::web
