#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/string.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <cstdint>

namespace bg {

template <typename T = void> class Scalar;

namespace util {
class symbol;
}

class ScalarType : public Type {
  public:
    using generic_scalar_view = Scalar<>;

    VISITOR_SUPPORT()

    bool is_scalar() const noexcept final { return true; }
    bool is_time_series() const noexcept final { return false; }
    virtual bool supports_equality() const noexcept { return false; }
    virtual bool supports_hash() const noexcept { return false; }
};

inline bool supports_scalar_equality(const ScalarType &type) noexcept {
    return type.supports_equality();
}

inline bool supports_scalar_hash(const ScalarType &type) noexcept {
    return type.supports_hash();
}

const ScalarType &int_type() noexcept;
const ScalarType &bool_type() noexcept;
const ScalarType &int64_type() noexcept;
const ScalarType &float_type() noexcept;
const ScalarType &double_type() noexcept;
const ScalarType &evaluation_time_type() noexcept;
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

template <> struct scalar_type_traits<std::int64_t> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<float> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<double> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<EvaluationTime> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<string> {
    static const ScalarType &type() noexcept;
};

template <> struct scalar_type_traits<util::symbol> {
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
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = true;
    static constexpr bool supports_time_series = true;
};

namespace details {

template <typename T> const ScalarType &scalar_type_for() {
    return cpp_scalar_type<T>();
}

} // namespace details

} // namespace bg
