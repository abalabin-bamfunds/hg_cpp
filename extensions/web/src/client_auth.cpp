#include <bg/web/client_auth.hpp>

#include <boost/url.hpp>

#include <charconv>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bg::web::details {

const service<auto(TS<Bundle<ClientAuthMatcher>>, string) -> TSS<string>> client_auth_registration_service{
    "client_auth_registration", {"matcher", "scope"}};
const service<auto(TSD<string, TS<Bundle<Auth>>>, string) -> void> client_credential_submission_service{
    "client_credential_submission", {"credentials", "scope"}};

namespace {

std::string normalize_origin_url(const boost::urls::url_view &url) {
    if (!url.has_authority() || url.has_userinfo() || url.host().empty()) {
        throw std::invalid_argument{"client auth URL must have a host and no user information"};
    }
    if (url.has_fragment()) throw std::invalid_argument{"client auth URL cannot have a fragment"};

    auto scheme = std::string{url.scheme()};
    for (auto &character : scheme) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    }
    if (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss") {
        throw std::invalid_argument{"client auth URL scheme must be http, https, ws, or wss"};
    }

    auto host = std::string{url.host()};
    for (auto &character : host) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    }
    if (url.host_type() == boost::urls::host_type::ipv6) {
        host = "[" + url.host_ipv6_address().to_string() + "]";
    }
    if (url.host_type() == boost::urls::host_type::ipvfuture) {
        throw std::invalid_argument{"client auth URL cannot use an IPvFuture host"};
    }

    auto normalized = scheme + "://" + host;
    if (url.has_port()) {
        const auto port_text = url.port();
        auto port = std::uint32_t{};
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (port_text.empty() || error != std::errc{} || end != port_text.data() + port_text.size() || port == 0 ||
            port > 65535) {
            throw std::invalid_argument{"client auth URL port must be between 1 and 65535"};
        }
        const auto default_port = (scheme == "http" || scheme == "ws") ? 80U : 443U;
        if (port != default_port) normalized += ":" + std::to_string(port);
    }
    return normalized;
}

} // namespace

std::string normalize_client_auth_origin(std::string_view origin) {
    const auto parsed = boost::urls::parse_uri(origin);
    if (!parsed) throw std::invalid_argument{"client auth origin is not a valid absolute URL"};

    const auto url = *parsed;
    if (url.has_query()) throw std::invalid_argument{"client auth origin cannot have a query"};
    if (!url.encoded_path().empty() && url.encoded_path() != "/") {
        throw std::invalid_argument{"client auth origin path must be empty or /"};
    }
    return normalize_origin_url(url);
}

std::string client_auth_origin_from_url(std::string_view value) {
    const auto parsed = boost::urls::parse_uri(value);
    if (!parsed) throw std::invalid_argument{"client auth request URL is invalid"};
    return normalize_origin_url(*parsed);
}

void validate_client_auth_regex(std::string_view pattern) {
    try {
        (void)std::regex{pattern.begin(), pattern.end(), std::regex::ECMAScript};
    } catch (const std::regex_error &error) {
        throw std::invalid_argument{std::string{"invalid client auth regular expression: "} + error.what()};
    }
}

} // namespace bg::web::details
