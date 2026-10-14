#pragma once

#include <bg/literal/scalar_literal.hpp>

#include <bg/node/output.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/ts/ts_output.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>
#include <bg/time_series/item_id.hpp>

#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace literal::details {

struct TslEntry {
    std::size_t index;
    const ScalarLiteral *value;
};

inline std::span<const ScalarLiteral> sequence(const ScalarLiteral &literal) {
    auto result = std::span<const ScalarLiteral>{};
    literal.visit(
        [](const ScalarLiteralItem &) {
            throw std::invalid_argument{"a time-series collection update requires a sequence"};
        },
        [&](std::span<const ScalarLiteral> values) { result = values; });
    return result;
}

inline std::size_t list_index(const ScalarLiteral &literal) {
    auto result = std::optional<std::size_t>{};
    literal.visit(
        [&](const ScalarLiteralItem &item) {
            item.visit(
                [&](const auto *source) {
                    using Scalar = std::remove_cvref_t<decltype(*source)>;
                    using Value = typename Scalar::scalar_type;
                    if constexpr (std::integral<Value> && !std::same_as<Value, bool>) {
                        const auto value = source->value();
                        if constexpr (std::signed_integral<Value>) {
                            if (value < 0) {
                                throw std::invalid_argument{"a TSL update index cannot be negative"};
                            }
                        }
                        if (!std::in_range<std::size_t>(value)) {
                            throw std::invalid_argument{"a TSL update index is out of range"};
                        }
                        result = static_cast<std::size_t>(value);
                    }
                },
                ddv::noop);
        },
        [](std::span<const ScalarLiteral>) {});

    if (!result) {
        throw std::invalid_argument{"a TSL update index must be an integral scalar"};
    }
    return *result;
}

inline TslEntry tsl_entry(const ScalarLiteral &literal) {
    auto result = std::optional<TslEntry>{};
    literal.visit(
        [](const ScalarLiteralItem &) {},
        [&](std::span<const ScalarLiteral> values) {
            if (values.size() != 2) {
                throw std::invalid_argument{"a TSL update entry requires an index and a value"};
            }
            result = TslEntry{list_index(values[0]), &values[1]};
        });

    if (!result) {
        throw std::invalid_argument{"a TSL update entry requires an index and a value"};
    }
    return *result;
}

template <typename Variable>
void apply_time_series(const bg::Out<Variable> &output, const ScalarLiteral &literal);

inline void apply_output(const bg::Out<bg::TS<bg::SCALAR>> &output, const ScalarLiteral &literal) {
    output.apply(literal);
}

template <typename Item, auto Size>
void apply_output(const bg::Out<bg::TSL<Item, Size>> &output, const ScalarLiteral &literal) {
    const auto values = sequence(literal);
    for (auto position = std::size_t{}; position < values.size(); ++position) {
        const auto current = tsl_entry(values[position]);
        if (current.index >= output.size() || current.index > std::numeric_limits<bg::item_id_t>::max()) {
            throw std::invalid_argument{"a TSL update index is out of range"};
        }

        for (auto previous = std::size_t{}; previous < position; ++previous) {
            if (tsl_entry(values[previous]).index == current.index) {
                throw std::invalid_argument{"a TSL update contains a duplicate index"};
            }
        }
    }

    for (const auto &value : values) {
        const auto current = tsl_entry(value);
        apply_time_series(output[current.index], *current.value);
    }
}

template <typename Output>
void apply_output(const Output &, const ScalarLiteral &) {
    throw std::invalid_argument{"this time-series type does not support literal updates"};
}

template <typename Variable>
void apply_time_series(const bg::Out<Variable> &output, const ScalarLiteral &literal) {
    output.visit([&](const auto *typed) { apply_output(*typed, literal); }, ddv::noop);
}

} // namespace literal::details

namespace literal {

template <typename Variable>
void apply(const bg::Out<Variable> &output, const ScalarLiteral &literal) {
    details::apply_time_series(output, literal);
}

} // namespace literal
