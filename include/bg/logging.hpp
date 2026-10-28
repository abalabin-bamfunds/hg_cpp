#pragma once

#include <bg/export.hpp>

#include <fmt/format.h>

#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace bg::logging {

enum class Level { trace, debug, info, warning, error, critical, off };

struct Config {
    Level level{Level::info};
    std::string file;
    bool std_err{false};
    std::string pattern{"{time} LOG_{level:<9} {logger:<12} {message}"};
    std::string time_pattern{"%H:%M:%S.%Qus"};
    bool utc{false};
};

BG_EXPORT void initialize(Config config = {});
BG_EXPORT void set_level(Level value);
BG_EXPORT Level level() noexcept;
BG_EXPORT bool enabled(Level value) noexcept;
BG_EXPORT void flush();

namespace details {

BG_EXPORT void submit(Level value, std::move_only_function<std::string()> format);

template <typename T> auto snapshot_argument(T &&value) {
    using Value = std::remove_cvref_t<T>;
    static_assert(!fmt::detail::is_named_arg<Value>::value,
                  "named fmt arguments cannot be deferred; pass owned values as positional arguments");

    if constexpr (std::same_as<Value, std::string_view>) {
        return std::string{value};
    } else if constexpr (std::is_array_v<Value> &&
                         std::same_as<std::remove_cv_t<std::remove_extent_t<Value>>, char>) {
        constexpr auto extent = std::extent_v<Value>;
        const auto text = std::string_view{value, extent};
        const auto size = !text.empty() && text.back() == '\0' ? text.size() - 1 : text.size();
        return std::string{text.data(), size};
    } else if constexpr (std::is_pointer_v<Value> &&
                         std::same_as<std::remove_cv_t<std::remove_pointer_t<Value>>, char>) {
        return value == nullptr ? std::string{} : std::string{value};
    } else {
        return Value{std::forward<T>(value)};
    }
}

template <Level VALUE, typename... Args> void log(fmt::format_string<Args...> pattern, Args &&...args) {
    if (!enabled(VALUE)) {
        return;
    }

    auto format = std::string{pattern.get().data(), pattern.get().size()};
    using Arguments = std::tuple<decltype(snapshot_argument(std::forward<Args>(args)))...>;
    auto arguments = Arguments{snapshot_argument(std::forward<Args>(args))...};
    submit(VALUE, [format = std::move(format), arguments = std::move(arguments)]() mutable {
        return std::apply(
            [&format](auto &...values) { return fmt::format(fmt::runtime(format), values...); }, arguments);
    });
}

} // namespace details
} // namespace bg::logging

namespace bg::log {

template <typename... Args> void trace(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::trace>(pattern, std::forward<Args>(args)...);
}

template <typename... Args> void debug(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::debug>(pattern, std::forward<Args>(args)...);
}

template <typename... Args> void info(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::info>(pattern, std::forward<Args>(args)...);
}

template <typename... Args> void warning(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::warning>(pattern, std::forward<Args>(args)...);
}

template <typename... Args> void error(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::error>(pattern, std::forward<Args>(args)...);
}

template <typename... Args> void critical(fmt::format_string<Args...> pattern, Args &&...args) {
    logging::details::log<logging::Level::critical>(pattern, std::forward<Args>(args)...);
}

} // namespace bg::log
