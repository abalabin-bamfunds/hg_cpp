#pragma once

#include <hg/scalar/bundle/scalar_type.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/scalar/list/scalar.hpp>
#include <hg/scalar/tuple/scalar.hpp>
#include <hg/scalar/value.hpp>
#include <hg/web/types.hpp>

#include <cstddef>
#include <ranges>
#include <string_view>

namespace hg::web::details {

enum class ClientAuthMethod { unsupported, anonymous, basic, digest, negotiate, ntlm, bearer, headers };

inline ClientAuthMethod client_auth_method(Scalar<Bundle<Auth>> value) {
    const auto &type = value.type();
    if (&type == &cpp_scalar_type<Bundle<AnonymousAuth>>()) return ClientAuthMethod::anonymous;
    if (&type == &cpp_scalar_type<Bundle<BasicAuth>>()) return ClientAuthMethod::basic;
    if (&type == &cpp_scalar_type<Bundle<DigestAuth>>()) return ClientAuthMethod::digest;
    if (&type == &cpp_scalar_type<Bundle<NegotiateAuth>>()) return ClientAuthMethod::negotiate;
    if (&type == &cpp_scalar_type<Bundle<NtlmAuth>>()) return ClientAuthMethod::ntlm;
    if (&type == &cpp_scalar_type<Bundle<BearerAuth>>()) return ClientAuthMethod::bearer;
    if (&type == &cpp_scalar_type<Bundle<HeaderAuth>>()) return ClientAuthMethod::headers;
    return ClientAuthMethod::unsupported;
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

} // namespace hg::web::details
