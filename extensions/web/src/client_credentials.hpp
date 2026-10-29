#pragma once

#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/web/types.hpp>

#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bg::web::details {

using ClientCredentialHeader = Tuple<string, string>;

struct ClientCredentialsSnapshot {
    std::string scheme;
    std::string username;
    std::string password;
    std::string token;
    std::vector<std::pair<std::string, std::string>> headers;
};

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

inline ClientCredentialsSnapshot snapshot_client_credentials(Scalar<Bundle<ClientCredentials>> value) {
    auto result = ClientCredentialsSnapshot{};
    result.scheme = std::string{scalar_cast<string>(value[0]).value()};
    result.username = std::string{scalar_cast<string>(value[1]).value()};
    result.password = std::string{scalar_cast<string>(value[2]).value()};
    result.token = std::string{scalar_cast<string>(value[3]).value()};
    const auto headers = scalar_cast<List<ClientCredentialHeader>>(value[4]);
    result.headers.reserve(headers.size());
    for (auto index = std::size_t{}; index < headers.size(); ++index) {
        const auto header = headers[index];
        result.headers.emplace_back(std::string{scalar_cast<string>(header.template get<0>()).value()},
                                    std::string{scalar_cast<string>(header.template get<1>()).value()});
    }
    return result;
}

} // namespace bg::web::details
