#pragma once

#include <hg/graph/wiring_template.hpp>
#include <hg/library/operators.hpp>
#include <hg/map.hpp>
#include <hg/operators.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/scalar/value.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/util/named_argument.hpp>
#include <hg/web/export.hpp>
#include <hg/web/http_client.hpp>
#include <hg/web/types.hpp>
#include <hg/web/websocket_client.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace hg::web {

struct ClientAuthRegex {
    std::string pattern;
};

namespace details {

template <template <typename, auto...> typename Field> struct ClientAuthMatcher {
    Field<string> pattern;
    Field<bool> regex;
};

extern HG_WEB_EXPORT const service<auto(TS<Bundle<ClientAuthMatcher>>, string) -> TSS<string>>
    client_auth_registration_service;

extern HG_WEB_EXPORT const service<auto(TSD<string, TS<Bundle<Auth>>>, string) -> void>
    client_credential_submission_service;

HG_WEB_EXPORT std::string normalize_client_auth_origin(std::string_view origin);
HG_WEB_EXPORT std::string client_auth_origin_from_url(std::string_view url);
HG_WEB_EXPORT void validate_client_auth_regex(std::string_view pattern);

inline ScalarValue<Bundle<ClientAuthMatcher>> client_auth_matcher(std::string_view pattern, bool regex) {
    using namespace hg::literals;
    auto memory = allocator{};
    return bundle<ClientAuthMatcher>(memory, "pattern"_n = pattern, "regex"_n = regex);
}

template <typename Graph, typename... Arguments>
void register_client_auth(std::string scope, std::string_view pattern, bool regex, Graph &&credential_graph,
                          Arguments &&...arguments) {
    using namespace hg::literals;
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

HG_WEB_EXPORT void register_client_transports(ServiceCatalogue &catalogue, std::string scope,
                                              HttpClientOptions http_options = {},
                                              WebSocketClientOptions websocket_options = {});

} // namespace hg::web
