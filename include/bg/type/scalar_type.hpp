#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/scalar/string.hpp>
#include <bg/type/type.hpp>

#include <cstdint>

namespace bg {

namespace util {
class symbol;
}

class ScalarType : public Type {
  public:
    VISITOR_SUPPORT()

    [[nodiscard]] bool is_scalar() const noexcept final { return true; }
    [[nodiscard]] bool is_time_series() const noexcept final { return false; }
    [[nodiscard]] virtual bool supports_equality() const noexcept { return false; }
    [[nodiscard]] virtual bool supports_hash() const noexcept { return false; }
};

[[nodiscard]] inline bool supports_scalar_equality(const ScalarType &type) noexcept {
    return type.supports_equality();
}

[[nodiscard]] inline bool supports_scalar_hash(const ScalarType &type) noexcept {
    return type.supports_hash();
}

[[nodiscard]] const ScalarType &int_type() noexcept;
[[nodiscard]] const ScalarType &bool_type() noexcept;
[[nodiscard]] const ScalarType &int64_type() noexcept;
[[nodiscard]] const ScalarType &float_type() noexcept;
[[nodiscard]] const ScalarType &double_type() noexcept;
[[nodiscard]] const ScalarType &evaluation_time_type() noexcept;
[[nodiscard]] const ScalarType &string_type() noexcept;
[[nodiscard]] const ScalarType &symbol_type() noexcept;

template <typename T> [[nodiscard]] const ScalarType &cpp_scalar_type() noexcept = delete;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<bool>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<int>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<std::int64_t>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<float>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<double>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<EvaluationTime>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<string>() noexcept;

template <> [[nodiscard]] const ScalarType &cpp_scalar_type<util::symbol>() noexcept;

namespace details {

// Defines where a C++ scalar declaration can be used and how it produces a semantic type.
struct scalar_type_properties {
    static constexpr bool is_collection = false;
    static constexpr bool supports_implicit_argument = true;
    static constexpr bool supports_time_series = true;
};

template <typename T> struct scalar_type_traits : scalar_type_properties {

    template <typename Scope> [[nodiscard]] static const ScalarType &get() noexcept { return cpp_scalar_type<T>(); }
};

template <typename Scope, typename T> [[nodiscard]] const ScalarType &scalar_type_for() {
    return scalar_type_traits<T>::template get<Scope>();
}

} // namespace details

} // namespace bg
