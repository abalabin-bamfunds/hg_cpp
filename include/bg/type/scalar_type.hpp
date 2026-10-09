#pragma once

#include <bg/scalar/builtins.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <cstddef>
#include <utility>

namespace bg {

template <typename T = void> class Scalar;

class ScalarType : public Type {
  public:
    using generic_scalar_view = Scalar<>;

    VISITOR_SUPPORT()

    bool is_scalar() const noexcept final { return true; }
    bool is_time_series() const noexcept final { return false; }
    void format_name(std::string &result) const override { result += "scalar"; }
    virtual bool supports_equality() const noexcept { return false; }
    virtual bool supports_hash() const noexcept { return false; }
};

class BuiltinScalarType : public ScalarType {
  public:
    VISITOR_SUPPORT()

    template <typename Visitor> decltype(auto) visit_builtin_type(Visitor &&visitor) const {
        static_assert(tp::size(details::builtin_scalar_declarations) == 8,
                      "Update BuiltinScalarType dispatch when adding a built-in scalar");

        switch (index_) {
        case 0:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<0, details::BuiltinScalarTypes>>();
        case 1:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<1, details::BuiltinScalarTypes>>();
        case 2:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<2, details::BuiltinScalarTypes>>();
        case 3:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<3, details::BuiltinScalarTypes>>();
        case 4:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<4, details::BuiltinScalarTypes>>();
        case 5:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<5, details::BuiltinScalarTypes>>();
        case 6:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<6, details::BuiltinScalarTypes>>();
        case 7:
            return std::forward<Visitor>(visitor).template operator()<tp::get_t<7, details::BuiltinScalarTypes>>();
        default:
            std::unreachable();
        }
    }

  protected:
    template <typename T>
        requires(tp::contains<T>(details::builtin_scalar_declarations))
    explicit BuiltinScalarType(tp::unit<T>) : index_{tp::find<T>(details::builtin_scalar_declarations)} {}

  private:
    std::size_t index_;
};

inline bool supports_scalar_equality(const ScalarType &type) noexcept {
    return type.supports_equality();
}

inline bool supports_scalar_hash(const ScalarType &type) noexcept {
    return type.supports_hash();
}

const ScalarType &int_type() noexcept;
const ScalarType &byte_type() noexcept;
const ScalarType &bool_type() noexcept;
const ScalarType &int64_type() noexcept;
const ScalarType &float_type() noexcept;
const ScalarType &double_type() noexcept;
const ScalarType &evaluation_time_type() noexcept;
const ScalarType &timedelta_type() noexcept;
const ScalarType &string_type() noexcept;
const ScalarType &symbol_type() noexcept;

// Maps a C++ scalar declaration to its stable semantic type object. Scalar providers specialize this trait and
// define type() in their library so every consumer observes the same object identity.
template <typename T> struct scalar_type_traits;

template <> struct scalar_type_traits<bool> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<int> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<byte> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<i64> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<float> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<double> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<date_time_t> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<timedelta_t> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<string> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<symbol> {
    static const ScalarType &type() noexcept;
};

template <typename T>
concept scalar_type_mapped = requires {
    { scalar_type_traits<T>::type() } -> std::same_as<const ScalarType &>;
};

template <scalar_type_mapped T>
const ScalarType &cpp_scalar_type() noexcept(noexcept(scalar_type_traits<T>::type())) {
    return scalar_type_traits<T>::type();
}

// Defines where a scalar declaration can be used. This is separate from semantic type identity and storage.
template <typename T> struct scalar_declaration_traits {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = true;
    static constexpr bool supports_time_series = true;
};

template <> struct scalar_declaration_traits<int> {
    static constexpr bool generic = false;
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = true;
    static constexpr bool supports_time_series = false;
};

namespace details {

template <typename T> const ScalarType &scalar_type_for() {
    return cpp_scalar_type<T>();
}

} // namespace details

} // namespace bg
