#pragma once

#include <bg/export.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/symbol.hpp>

#include <optional>

namespace literal {

class ScalarLiteralItem;
template <typename Leaf> class Literal;
using ScalarLiteral = Literal<ScalarLiteralItem>;

struct Sentinel {
    bg::symbol name;
    const ScalarLiteral *argument = nullptr;

    Sentinel operator()(const ScalarLiteral &value) const noexcept { return {name, &value}; }

    static BG_EXPORT std::optional<Sentinel> from(const bg::ScalarData &value);
    static BG_EXPORT std::optional<Sentinel> from(const ScalarLiteralItem &value);
    static BG_EXPORT std::optional<Sentinel> from(const ScalarLiteral &value);
};

extern BG_EXPORT const Sentinel SKIP;
extern BG_EXPORT const Sentinel INVALID;
extern BG_EXPORT const Sentinel REMOVE;
extern BG_EXPORT const Sentinel REMOVED;

} // namespace literal

namespace bg {

template <> struct scalar_type_traits<literal::Sentinel> {
    static BG_EXPORT const ScalarType &type() noexcept;
};

template <> struct scalar_declaration_traits<literal::Sentinel> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = false;
};

extern template BG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::Sentinel>();

} // namespace bg
