#pragma once

#include <bg/graph/wiring_template.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/value.hpp>
#include <bg/service.hpp>
#include <bg/service_catalogue.hpp>
#include <bg/util/named_argument.hpp>
#include <bg/web/export.hpp>
#include <bg/web/http_client.hpp>
#include <bg/web/types.hpp>
#include <bg/web/websocket_client.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace bg::web {

struct ClientAuthRegex {
    std::string pattern;
};

namespace details {

template <template <typename, auto...> typename Field> struct ClientAuthMatcher {
    Field<string> pattern;
    Field<bool> regex;
};

extern BG_WEB_EXPORT const service<auto(TS<Bundle<ClientAuthMatcher>>, string) -> TSS<string>>
    client_auth_registration_service;

extern BG_WEB_EXPORT const service<auto(TSD<string, TS<Bundle<Auth>>>, string) -> void>
    client_credential_submission_service;

BG_WEB_EXPORT std::string normalize_client_auth_origin(std::string_view origin);
BG_WEB_EXPORT std::string client_auth_origin_from_url(std::string_view url);
BG_WEB_EXPORT void validate_client_auth_regex(std::string_view pattern);

inline ScalarValue<Bundle<ClientAuthMatcher>> client_auth_matcher(std::string_view pattern, bool regex) {
    using namespace bg::literals;
    auto memory = allocator{};
    return bundle<ClientAuthMatcher>(memory, "pattern"_n = pattern, "regex"_n = regex);
}

template <typename Graph, typename... Arguments>
void register_client_auth(std::string scope, std::string_view pattern, bool regex, Graph &&credential_graph,
                          Arguments &&...arguments) {
    using namespace bg::literals;
    const auto registration_id = service_client_id();
    client_auth_registration_service.client_requests(
        registration_id, operators::const_(client_auth_matcher(pattern, regex)), scope);
    const auto origins = client_auth_registration_service.client_replies(registration_id, scope);
    const auto credentials = map(std::forward<Graph>(credential_graph), origins, "__key_arg__"_n = 0,
                                pass_through(std::forward<Arguments>(arguments))...);
    client_credential_submission_service.client_requests(registration_id, credentials, scope);
}

} // namespace details

template <typename Graph, typename... Arguments>
void register_client_auth(std::string scope, std::string_view exact_origin, Graph &&credential_graph,
                          Arguments &&...arguments) {
    const auto normalized_origin = details::normalize_client_auth_origin(exact_origin);
    details::register_client_auth(std::move(scope), normalized_origin, false,
                                  std::forward<Graph>(credential_graph), std::forward<Arguments>(arguments)...);
}

template <typename Graph, typename... Arguments>
void register_client_auth(std::string scope, ClientAuthRegex matcher, Graph &&credential_graph,
                          Arguments &&...arguments) {
    details::validate_client_auth_regex(matcher.pattern);
    details::register_client_auth(std::move(scope), matcher.pattern, true,
                                  std::forward<Graph>(credential_graph), std::forward<Arguments>(arguments)...);
}

BG_WEB_EXPORT void register_client_transports(ServiceCatalogue &catalogue, std::string scope,
                                              HttpClientOptions http_options = {},
                                              WebSocketClientOptions websocket_options = {});

} // namespace bg::web
