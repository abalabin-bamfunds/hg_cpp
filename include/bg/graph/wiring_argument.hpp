#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace bg::details {

struct wiring_argument_access;

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

} // namespace bg::details

namespace bg {

class WiringArgument {
    struct TimeSeriesArgument {
        explicit TimeSeriesArgument(WiringPoint value) : point{value}, resolution_type{&value.type()} {}

        WiringPoint point;
        const TimeSeriesType *resolution_type;
    };

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
    WiringArgument(const WiringArgument &other)
        : value_{copy_value(other)}, name_{other.name_},
          variadic_capture_mode_{other.variadic_capture_mode_} {
        if (auto *scalar = std::get_if<ScalarArgument>(&value_); scalar != nullptr && other.scalar_has_value()) {
            scalar->owned.emplace(other.scalar());
            scalar->data = nullptr;
            scalar->view = nullptr;
        }
    }

    WiringArgument(WiringArgument &&) noexcept = default;
    WiringArgument &operator=(const WiringArgument &) = delete;
    WiringArgument &operator=(WiringArgument &&) noexcept = default;

    WiringArgument(WiringPoint point) : value_(TimeSeriesArgument{point}) {}

    template <typename T>
        requires details::is_typed_wiring_point<T>
    WiringArgument(T point) : WiringArgument{static_cast<WiringPoint>(point)} {}

    template <typename T>
        requires(details::wiring_point_argument<T> && !details::is_typed_wiring_point<T> &&
                 !std::same_as<std::remove_cvref_t<T>, WiringPoint>)
    WiringArgument(const T &value) : WiringArgument{static_cast<WiringPoint>(value)} {}

    template <typename T>
        requires details::supported_scalar_argument<T>
    WiringArgument(T &&value) : value_(make_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires details::supported_optional_scalar_argument<T>
    WiringArgument(T &&value) : value_(make_optional_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires details::supported_scalar_value_argument<T>
    WiringArgument(const T &value)
        : value_(ScalarArgument{
              cpp_scalar_type<typename details::scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(),
              cpp_scalar_runtime_type<typename details::scalar_value_argument<std::remove_cvref_t<T>>::scalar_type>(),
              nullptr, &details::scalar_data(value)}) {}

    template <util::compile_time_string Name, typename T>
        requires requires(T &&value) { WiringArgument{std::forward<T>(value)}; }
    WiringArgument(details::named_argument<Name, T> value) : WiringArgument{std::move(value.value)} {
        name_ = util::symbol{Name.view()};
        variadic_capture_mode_ = details::VariadicMode::none;
    }

    bool is_time_series() const noexcept { return std::holds_alternative<TimeSeriesArgument>(value_); }
    bool is_scalar() const noexcept { return !is_time_series(); }
    bool present() const noexcept {
        const auto *scalar = std::get_if<ScalarArgument>(&value_);
        return scalar == nullptr || scalar->owned.has_value() || scalar->data != nullptr || scalar->view != nullptr;
    }
    std::optional<util::symbol> name() const noexcept { return name_; }
    const Type &type() const noexcept {
        if (is_time_series()) {
            return *std::get<TimeSeriesArgument>(value_).resolution_type;
        }
        return scalar_type();
    }

    WiringPoint time_series() const {
        if (!is_time_series()) {
            throw std::logic_error{"a wiring argument does not contain a time series"};
        }
        return std::get<TimeSeriesArgument>(value_).point;
    }

    Scalar<> scalar() const {
        if (!is_scalar() || !scalar_has_value()) {
            throw std::logic_error{"a wiring argument does not contain a scalar value"};
        }

        if (const auto *value = scalar_view()) {
            return Scalar<>::from(value->runtime_type(), value->data());
        }
        return Scalar<>::from(scalar_runtime_type(), scalar_data());
    }

  private:
    static WiringArgument borrow(const WiringArgument &other) {
        auto result = WiringArgument{copy_value(other)};
        result.name_ = other.name_;
        result.variadic_capture_mode_ = other.variadic_capture_mode_;
        return result;
    }

    void clear_name() noexcept { name_.reset(); }
    void replace_time_series(WiringPoint point) { std::get<TimeSeriesArgument>(value_).point = point; }
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

    static std::variant<TimeSeriesArgument, ScalarArgument> copy_value(const WiringArgument &other) {
        if (other.is_time_series()) {
            return std::get<TimeSeriesArgument>(other.value_);
        }
        return ScalarArgument{other.scalar_type(), other.scalar_runtime_type(), other.scalar_data(),
                              other.scalar_view()};
    }

    friend struct details::wiring_argument_access;

    WiringArgument clone(allocator &memory) const {
        if (is_time_series() || !scalar_has_value()) {
            return *this;
        }

        auto result = WiringArgument{ScalarArgument{scalar_type(), scalar_runtime_type()}};
        std::get<ScalarArgument>(result.value_).owned.emplace(scalar(), memory);
        result.name_ = name_;
        result.variadic_capture_mode_ = variadic_capture_mode_;
        return result;
    }

    explicit WiringArgument(ScalarArgument scalar) : value_(std::move(scalar)) {}
    explicit WiringArgument(std::variant<TimeSeriesArgument, ScalarArgument> value) : value_(std::move(value)) {}

    template <typename T> static ScalarArgument make_scalar_argument(T &&value) {
        using Source = std::remove_cvref_t<T>;
        using Scalar = details::canonical_scalar_type_t<Source>;

        if constexpr (scalar_source<Source>) {
            return ScalarArgument{std::forward<T>(value)};
        } else {
            return ScalarArgument{cpp_scalar_type<Scalar>(), cpp_scalar_runtime_type<Scalar>(), std::addressof(value)};
        }
    }

    template <typename T> static ScalarArgument make_optional_scalar_argument(T &&value) {
        using Source = typename details::optional_scalar_argument<std::remove_cvref_t<T>>::value_type;
        using Scalar = details::canonical_scalar_type_t<Source>;

        if (!value) {
            return ScalarArgument{cpp_scalar_type<Scalar>(), cpp_scalar_runtime_type<Scalar>()};
        }
        return make_scalar_argument(*value);
    }

    std::variant<TimeSeriesArgument, ScalarArgument> value_;
    std::optional<util::symbol> name_;
    details::VariadicMode variadic_capture_mode_{details::VariadicMode::none};
};

} // namespace bg

namespace bg::details {

struct wiring_argument_access {
    static WiringArgument borrow(const WiringArgument &argument) { return WiringArgument::borrow(argument); }
    static void clear_name(WiringArgument &argument) noexcept { argument.clear_name(); }
    static void replace_time_series(WiringArgument &argument, WiringPoint point) {
        argument.replace_time_series(point);
    }
    static VariadicMode variadic_capture_mode(const WiringArgument &argument) noexcept {
        return argument.variadic_capture_mode_;
    }
    static void set_variadic_name(WiringArgument &argument, util::symbol name, VariadicMode mode) noexcept {
        argument.name_ = name;
        argument.variadic_capture_mode_ = mode;
    }
    static const ScalarRuntimeType &scalar_runtime_type(const WiringArgument &argument) noexcept {
        return argument.scalar_runtime_type();
    }
    static WiringArgument clone(const WiringArgument &argument, allocator &memory) { return argument.clone(memory); }
};

template <typename T>
concept graph_code_argument_value =
    wiring_point_argument<T> || supported_scalar_argument<T> || supported_optional_scalar_argument<T> ||
    supported_scalar_value_argument<T> || std::same_as<std::remove_cvref_t<T>, WiringArgument>;

template <typename T>
concept graph_code_argument = graph_code_argument_value<T> ||
                              (is_named_argument_v<T> && graph_code_argument_value<decltype(std::declval<T>().value)>);

} // namespace bg::details
