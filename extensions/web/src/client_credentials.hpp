#pragma once

#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/web/types.hpp>

#include <cstddef>
#include <ranges>
#include <string_view>

namespace bg::web::details {

enum class ClientAuthKind { unsupported, anonymous, basic, digest, negotiate, ntlm, bearer, headers };

inline ClientAuthKind client_auth_kind(Scalar<Bundle<Auth>> value) {
    const auto &type = value.type();
    if (&type == &cpp_scalar_type<Bundle<AnonymousAuth>>()) return ClientAuthKind::anonymous;
    if (&type == &cpp_scalar_type<Bundle<BasicAuth>>()) return ClientAuthKind::basic;
    if (&type == &cpp_scalar_type<Bundle<DigestAuth>>()) return ClientAuthKind::digest;
    if (&type == &cpp_scalar_type<Bundle<NegotiateAuth>>()) return ClientAuthKind::negotiate;
    if (&type == &cpp_scalar_type<Bundle<NtlmAuth>>()) return ClientAuthKind::ntlm;
    if (&type == &cpp_scalar_type<Bundle<BearerAuth>>()) return ClientAuthKind::bearer;
    if (&type == &cpp_scalar_type<Bundle<HeaderAuth>>()) return ClientAuthKind::headers;
    return ClientAuthKind::unsupported;
}

inline bool valid_bearer_token(std::string_view value) noexcept {
    const auto padding = value.find('=');
    const auto token = value.substr(0, padding);
    if (token.empty()) return false;
    const auto valid_token_character = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
               character == '~' || character == '+' || character == '/';
    };
    if (!std::ranges::all_of(token, valid_token_character)) return false;
    if (padding == std::string_view::npos) return true;
    return std::ranges::all_of(value.substr(padding), [](char character) { return character == '='; });
}

inline bool contains_nul(std::string_view value) noexcept { return value.find('\0') != std::string_view::npos; }

} // namespace bg::web::details
