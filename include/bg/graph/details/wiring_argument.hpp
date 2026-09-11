#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace bg::details {

template <typename T>
inline constexpr bool supported_scalar_argument =
    scalar_type_traits<std::remove_cvref_t<T>>::supports_implicit_argument &&
    requires { cpp_scalar_type<canonical_scalar_type_t<T>>(); };

template <typename T> struct optional_scalar_argument : std::false_type {};

template <typename T>
struct optional_scalar_argument<std::optional<T>> : std::bool_constant<supported_scalar_argument<T>> {
    using value_type = T;
};

template <typename T>
inline constexpr bool supported_optional_scalar_argument = optional_scalar_argument<std::remove_cvref_t<T>>::value;

template <typename T> struct scalar_value_argument : std::false_type {};

template <typename T> struct scalar_value_argument<ScalarValue<T>> : std::bool_constant<!std::same_as<T, SCALAR>> {
    using scalar_type = T;
};

template <typename T>
inline constexpr bool supported_scalar_value_argument = scalar_value_argument<std::remove_cvref_t<T>>::value;

template <typename T>
concept wiring_point_argument = std::same_as<std::remove_cvref_t<T>, WiringPoint> || is_typed_wiring_point<T> ||
                                requires(const std::remove_cvref_t<T> &value) { static_cast<WiringPoint>(value); };

template <typename T>
concept graph_code_argument = wiring_point_argument<T> || supported_scalar_argument<T> ||
                              supported_optional_scalar_argument<T> || supported_scalar_value_argument<T>;

class wiring_argument {
    struct ScalarArgument {
        template <scalar_source Source>
        explicit ScalarArgument(Source &&source) : owned{std::in_place, std::forward<Source>(source)} {}

        ScalarArgument(const ScalarType &scalar_type, const ScalarRuntimeType &scalar_runtime_type,
                       const void *scalar_data = nullptr, const details::ScalarData *scalar_view = nullptr)
            : type{&scalar_type}, runtime_type{&scalar_runtime_type}, data{scalar_data}, view{scalar_view} {}

        std::optional<ScalarValue<>> owned;
        const ScalarType *type{};
        const ScalarRuntimeType *runtime_type{};
        const void *data{};
        const details::ScalarData *view{};
    };

  public:
    wiring_argument(WiringPoint point) : value_(point) {}

    template <typename T>
        requires is_typed_wiring_point<T>
    wiring_argument(T point) : value_(static_cast<WiringPoint>(point)) {}

    template <typename T>
        requires(wiring_point_argument<T> && !is_typed_wiring_point<T> &&
                 !std::same_as<std::remove_cvref_t<T>, WiringPoint>)
    wiring_argument(const T &value) : value_(static_cast<WiringPoint>(value)) {}

    template <typename T>
        requires supported_scalar_argument<T>
    wiring_argument(T &&value) : value_(make_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires supported_optional_scalar_argument<T>
    wiring_argument(T &&value) : value_(make_optional_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires supported_scalar_value_argument<T>
    wiring_argument(const T &value)
        : value_(ScalarArgument{
              cpp_scalar_type<typename scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(),
              cpp_scalar_runtime_type<typename scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(), nullptr,
              &details::scalar_data(value)}) {}

    bool is_wiring_point() const noexcept { return std::holds_alternative<WiringPoint>(value_); }

    bool present() const noexcept {
        const auto *scalar = std::get_if<ScalarArgument>(&value_);
        return scalar == nullptr || scalar->owned.has_value() || scalar->data != nullptr || scalar->view != nullptr;
    }

    bool is_scalar() const noexcept { return std::holds_alternative<ScalarArgument>(value_); }

    WiringPoint wiring_point() const { return std::get<WiringPoint>(value_); }

    void wiring_point(WiringPoint point) { value_ = point; }

    const ScalarType &scalar_type() const noexcept {
        const auto &scalar = std::get<ScalarArgument>(value_);
        return scalar.owned ? scalar.owned->type() : *scalar.type;
    }

    const ScalarRuntimeType &scalar_runtime_type() const noexcept {
        const auto &scalar = std::get<ScalarArgument>(value_);
        return scalar.owned ? scalar.owned->runtime_type() : *scalar.runtime_type;
    }

    const void *scalar_data() const noexcept { return std::get<ScalarArgument>(value_).data; }

    const details::ScalarData *scalar_view() const noexcept {
        const auto &scalar = std::get<ScalarArgument>(value_);
        return scalar.owned ? &details::scalar_data(*scalar.owned) : scalar.view;
    }

    bool scalar_has_value() const noexcept { return scalar_data() != nullptr || scalar_view() != nullptr; }

    const Type &type() const noexcept;

  private:
    template <typename T> static ScalarArgument make_scalar_argument(T &&value) {
        using Source = std::remove_cvref_t<T>;
        using Scalar = canonical_scalar_type_t<Source>;

        if constexpr (scalar_source<Source>) {
            return ScalarArgument{std::forward<T>(value)};
        } else {
            return ScalarArgument{cpp_scalar_type<Scalar>(), cpp_scalar_runtime_type<Scalar>(), std::addressof(value)};
        }
    }

    template <typename T> static ScalarArgument make_optional_scalar_argument(T &&value) {
        using Source = typename optional_scalar_argument<std::remove_cvref_t<T>>::value_type;
        using Scalar = canonical_scalar_type_t<Source>;

        if (!value) {
            return ScalarArgument{cpp_scalar_type<Scalar>(), cpp_scalar_runtime_type<Scalar>()};
        }
        return make_scalar_argument(*value);
    }

    std::variant<WiringPoint, ScalarArgument> value_;
};

} // namespace bg::details
