#pragma once

#include <bg/export.hpp>
#include <bg/scalar/data.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/storage_traits.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace literal {

class ScalarLiteralItem;
template <typename Leaf> class Literal;
using ScalarLiteral = Literal<ScalarLiteralItem>;

struct TaggedLiteral;

class Sentinel {
  public:
    explicit Sentinel(bg::symbol value) noexcept : name{std::move(value)} {}

    bg::symbol name;

    Sentinel(const Sentinel &) = delete;
    Sentinel &operator=(const Sentinel &) = delete;
    Sentinel(Sentinel &&) = delete;
    Sentinel &operator=(Sentinel &&) = delete;

    bool operator==(const Sentinel &other) const noexcept { return this == &other; }

    TaggedLiteral operator()(const ScalarLiteral &value) const noexcept;

    static BG_EXPORT const Sentinel *from(const bg::ScalarData &value) noexcept;
    static BG_EXPORT const Sentinel *from(const ScalarLiteralItem &value);
    static BG_EXPORT const Sentinel *from(const ScalarLiteral &value);
};

struct TaggedLiteral {
    const Sentinel &sentinel;
    const ScalarLiteral &argument;

    static BG_EXPORT std::optional<TaggedLiteral> from(const bg::ScalarData &value);
    static BG_EXPORT std::optional<TaggedLiteral> from(const ScalarLiteralItem &value);
    static BG_EXPORT std::optional<TaggedLiteral> from(const ScalarLiteral &value);
};

inline TaggedLiteral Sentinel::operator()(const ScalarLiteral &value) const noexcept {
    return TaggedLiteral{*this, value};
}

extern BG_EXPORT const Sentinel SKIP;
extern BG_EXPORT const Sentinel INVALID;
extern BG_EXPORT const Sentinel REMOVE;
extern BG_EXPORT const Sentinel REMOVED;

} // namespace literal

namespace bg {

template <> struct scalar_storage_traits<literal::Sentinel> {
    using stored_type = const literal::Sentinel *;
    using value_type = const literal::Sentinel &;

    static constexpr bool supports_equality = true;
    static constexpr bool supports_hash = true;
    static constexpr bool supports_alternative_layouts = false;
    static constexpr bool uses_shared_handle = false;
    static constexpr bool uses_variable_memory = false;
    static constexpr bool requires_finalization = false;
    static constexpr bool assign_uses_memory_resource = false;

    static value_type read(stored_type value) noexcept { return *value; }
    static std::size_t hash(value_type value) noexcept { return std::hash<const literal::Sentinel *>{}(&value); }
    static bool equals(value_type left, value_type right) noexcept { return left == right; }
    static void construct(stored_type *destination, value_type value, allocator &) noexcept {
        std::construct_at(destination, &value);
    }
    static void assign(stored_type &destination, value_type value, allocator &) noexcept { destination = &value; }
    static void destroy(stored_type *value, allocator &) noexcept { std::destroy_at(value); }
};

template <> struct scalar_type_traits<literal::Sentinel> {
    static BG_EXPORT const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<literal::TaggedLiteral> {
    static BG_EXPORT const ScalarType &type() noexcept;
};

template <> struct scalar_declaration_traits<literal::Sentinel> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = false;
};

template <> struct scalar_declaration_traits<literal::TaggedLiteral> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = false;
    static constexpr bool supports_time_series = false;
};

extern template BG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::Sentinel>();
extern template BG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::TaggedLiteral>();

} // namespace bg
