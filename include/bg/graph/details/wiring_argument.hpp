#pragma once

#include <bg/graph/details/free_time_series_bundle.hpp>
#include <bg/graph/details/free_time_series_list.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/scalar/cpp_storage_representation.hpp>
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
    requires { cpp_scalar_type<std::remove_cvref_t<T>>(); };

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
concept graph_code_argument =
    std::same_as<std::remove_cvref_t<T>, WiringPoint> || is_typed_wiring_point<T> || supported_scalar_argument<T> ||
    supported_optional_scalar_argument<T> || supported_scalar_value_argument<T> || free_time_series_list<T> ||
    free_time_series_bundle<T>;

class wiring_argument {
    struct ScalarArgument {
        const ScalarType *type;
        const ScalarStorageRepresentation *storage_representation;
        const void *data;
        const ScalarInputView *view;
    };

  public:
    wiring_argument(WiringPoint point) : value_(point) {}

    template <typename T>
        requires is_typed_wiring_point<T>
    wiring_argument(T point) : value_(static_cast<WiringPoint>(point)) {}

    template <free_time_series_list T> wiring_argument(const T &value) : value_(value.wiring_argument()) {}

    template <free_time_series_bundle T> wiring_argument(const T &value) : value_(value.wiring_argument()) {}

    template <typename T>
        requires supported_scalar_argument<T>
    wiring_argument(T &&value)
        : value_(ScalarArgument{&cpp_scalar_type<std::remove_cvref_t<T>>(),
                                &cpp_scalar_storage_representation<std::remove_cvref_t<T>>(), std::addressof(value),
                                nullptr}) {}

    template <typename T>
        requires supported_optional_scalar_argument<T>
    wiring_argument(T &&value)
        : value_(
              ScalarArgument{&cpp_scalar_type<typename optional_scalar_argument<std::remove_cvref_t<T>>::value_type>(),
                             &cpp_scalar_storage_representation<
                                 typename optional_scalar_argument<std::remove_cvref_t<T>>::value_type>(),
                             value ? std::addressof(*value) : nullptr, nullptr}) {}

    template <typename T>
        requires supported_scalar_value_argument<T>
    wiring_argument(const T &value)
        : value_(ScalarArgument{
              &cpp_scalar_type<typename scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(),
              &cpp_scalar_storage_representation<typename scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(),
              nullptr, &value.input_view()}) {}

    [[nodiscard]] bool is_wiring_point() const noexcept { return std::holds_alternative<WiringPoint>(value_); }

    [[nodiscard]] bool is_free_time_series_list() const noexcept {
        return std::holds_alternative<FreeTimeSeriesListArgument>(value_);
    }

    [[nodiscard]] bool is_free_time_series_bundle() const noexcept {
        return std::holds_alternative<FreeTimeSeriesBundleArgument>(value_);
    }

    [[nodiscard]] WiringPoint wiring_point() const { return std::get<WiringPoint>(value_); }

    void wiring_point(WiringPoint point) { value_ = point; }

    [[nodiscard]] const FreeTimeSeriesListArgument &free_time_series_list() const {
        return std::get<FreeTimeSeriesListArgument>(value_);
    }

    [[nodiscard]] const FreeTimeSeriesBundleArgument &free_time_series_bundle() const {
        return std::get<FreeTimeSeriesBundleArgument>(value_);
    }

    [[nodiscard]] const ScalarType &scalar_type() const noexcept { return *std::get<ScalarArgument>(value_).type; }

    [[nodiscard]] const ScalarStorageRepresentation &scalar_storage_representation() const noexcept {
        return *std::get<ScalarArgument>(value_).storage_representation;
    }

    [[nodiscard]] const void *scalar_data() const noexcept { return std::get<ScalarArgument>(value_).data; }

    [[nodiscard]] const ScalarInputView *scalar_view() const noexcept { return std::get<ScalarArgument>(value_).view; }

    [[nodiscard]] bool scalar_has_value() const noexcept {
        return scalar_data() != nullptr || scalar_view() != nullptr;
    }

  private:
    std::variant<WiringPoint, ScalarArgument, FreeTimeSeriesListArgument, FreeTimeSeriesBundleArgument> value_;
};

} // namespace bg::details
