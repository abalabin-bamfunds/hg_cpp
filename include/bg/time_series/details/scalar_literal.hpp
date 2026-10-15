#pragma once

#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/literal.hpp>

#include <concepts>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg::details::scalar_literal {

using Literal = literal::ScalarLiteral;

struct Pair {
    const Literal *first;
    const Literal *second;
};

inline std::span<const Literal> sequence(const Literal &value) {
    return value.visit(
        [](const literal::ScalarLiteralItem &) -> std::span<const Literal> {
            throw std::invalid_argument{"a time-series collection update requires a sequence"};
        },
        [](std::span<const Literal> values) { return values; }).value();
}

inline Pair pair(const Literal &value, const char *message) {
    return value.visit(
        [message](const literal::ScalarLiteralItem &) -> Pair { throw std::invalid_argument{message}; },
        [message](std::span<const Literal> values) -> Pair {
            if (values.size() != 2) {
                throw std::invalid_argument{message};
            }
            return {&values[0], &values[1]};
        }).value();
}

inline std::size_t list_index(const Literal &value) {
    auto result = std::optional<std::size_t>{};
    value.visit(
        [&](const literal::ScalarLiteralItem &item) {
            item.visit(
                [&](const auto *source) {
                    using Scalar = std::remove_cvref_t<decltype(*source)>;
                    using Value = typename Scalar::scalar_type;
                    if constexpr (std::integral<Value> && !std::same_as<Value, bool>) {
                        const auto index = source->value();
                        if constexpr (std::signed_integral<Value>) {
                            if (index < 0) {
                                throw std::invalid_argument{"a TSL update index cannot be negative"};
                            }
                        }
                        if (!std::in_range<std::size_t>(index)) {
                            throw std::invalid_argument{"a TSL update index is out of range"};
                        }
                        result = static_cast<std::size_t>(index);
                    }
                },
                ddv::noop);
        },
        [](std::span<const Literal>) {});

    if (!result) {
        throw std::invalid_argument{"a TSL update index must be an integral scalar"};
    }
    return *result;
}

inline std::string_view field_name(const Literal &value) {
    return value.visit(
        [](const literal::ScalarLiteralItem &item) {
            return item.visit([](const auto *source) -> std::string_view {
                using Scalar = std::remove_cvref_t<decltype(*source)>;
                if constexpr (std::same_as<typename Scalar::scalar_type, string>) {
                    return source->value();
                } else if constexpr (std::same_as<typename Scalar::scalar_type, symbol>) {
                    return source->value().text();
                } else {
                    throw std::invalid_argument{"a TSB field name must be a string or symbol"};
                }
            }).value();
        },
        [](std::span<const Literal>) -> std::string_view {
            throw std::invalid_argument{"a TSB field name must be a string or symbol"};
        }).value();
}

template <typename Function>
void with_scalar_data(const ScalarRuntimeType &runtime_type, const Literal &value, Function &&function) {
    value.visit(
        [&](const literal::ScalarLiteralItem &item) {
            item.visit([&](const auto *source) { std::invoke(function, details::scalar_data(*source)); });
        },
        [&](std::span<const Literal>) {
            auto owned = ScalarValue<>::from_literal(runtime_type, value);
            std::invoke(function, details::scalar_data(owned));
        });
}

} // namespace bg::details::scalar_literal
