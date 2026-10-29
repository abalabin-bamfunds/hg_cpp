#pragma once

#include <bg/allocator.hpp>
#include <bg/export.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type.hpp>
#include <bg/util/named_argument.hpp>

#include <concepts>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg::details {

template <typename T>
inline constexpr bool supported_scalar_argument =
    scalar_declaration_traits<std::remove_cvref_t<T>>::supports_implicit_argument &&
    requires { cpp_scalar_type<canonical_scalar_type_t<T>>(); };

template <typename T> struct optional_scalar_argument {
    static constexpr bool value = false;
};

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

template <typename T> struct graph_scalar_view : std::false_type {};

template <typename T> struct graph_scalar_view<Scalar<T>> : std::true_type {
    using scalar_type = T;
};

template <typename T>
concept graph_scalar_view_argument = graph_scalar_view<std::remove_cvref_t<T>>::value;

template <typename T>
concept wiring_point_argument = std::same_as<std::remove_cvref_t<T>, WiringPoint> || is_typed_wiring_point<T> ||
                                requires(const std::remove_cvref_t<T> &value) { static_cast<WiringPoint>(value); };

struct WiringArgumentExpansion {
    WiringPoint bundle;
    VariadicMode mode;
    WiringAnnotations annotations;
};

template <typename T>
concept opaque_source_argument = [] {
    using Source = std::remove_cvref_t<T>;
    if constexpr (std::same_as<Source, WiringArgument> || std::same_as<Source, WiringArgumentExpansion>) {
        return false;
    } else if constexpr (requires { cpp_scalar_type<canonical_scalar_type_t<Source>>(); } ||
                         requires { typename optional_scalar_argument<Source>::value_type; } ||
                         requires { typename scalar_value_argument<Source>::scalar_type; } ||
                         graph_scalar_view_argument<T> || wiring_point_argument<T> || graph_argument_facade<T> ||
                         is_named_argument_v<T> || is_opaque_v<Source> || std::same_as<Source, GraphArgument> ||
                         std::convertible_to<Source, std::span<const WiringArgument>> ||
                         std::convertible_to<Source, std::span<const WiringPoint>>) {
        return false;
    } else if constexpr (!std::copy_constructible<Source>) {
        return false;
    } else {
        return true;
    }
}();

} // namespace bg::details

namespace bg {

class Signature;

class WiringArgument {
    struct TimeSeriesArgument {
        explicit TimeSeriesArgument(WiringPoint value) : point{value}, resolution_type{&value.type()} {}

        WiringPoint point;
        const TimeSeriesType *resolution_type;
        details::WiringAnnotations annotations{};
    };

    struct ScalarArgument {
        template <scalar_source Source>
        explicit ScalarArgument(Source &&source) : owned{std::in_place, std::forward<Source>(source)} {}

        explicit ScalarArgument(ScalarValue<> source) : owned{std::move(source)} {}

        ScalarArgument(const ScalarType &scalar_type, const ScalarRuntimeType &scalar_runtime_type,
                       const void *scalar_data = nullptr, const details::ScalarData *scalar_view = nullptr)
            : type{&scalar_type}, runtime_type{&scalar_runtime_type}, data{scalar_data}, view{scalar_view} {}

        std::optional<ScalarValue<>> owned;
        const ScalarType *type{};
        const ScalarRuntimeType *runtime_type{};
        const void *data{};
        const details::ScalarData *view{};
    };

    struct OpaqueScalarCandidate {
        ScalarArgument scalar;
    };

    static_assert(sizeof(OpaqueScalarCandidate) == sizeof(ScalarArgument));
    static_assert(sizeof(std::variant<TimeSeriesArgument, ScalarArgument, OpaqueScalarCandidate, GraphArgument>) ==
                  sizeof(std::variant<TimeSeriesArgument, ScalarArgument, GraphArgument>));

  public:
    WiringArgument(const WiringArgument &other)
        : value_{copy_value(other)}, name_{other.name_}, variadic_capture_mode_{other.variadic_capture_mode_},
          match_fixed_parameter_by_name_{other.match_fixed_parameter_by_name_} {
        if (is_scalar() && other.scalar_has_value()) {
            auto &scalar = scalar_argument();
            scalar.owned.emplace(other.scalar());
            scalar.data = nullptr;
            scalar.view = nullptr;
        }
    }

    WiringArgument(WiringArgument &&) noexcept = default;
    WiringArgument &operator=(const WiringArgument &) = delete;
    WiringArgument &operator=(WiringArgument &&) noexcept = default;

    WiringArgument(WiringPoint point) : value_(TimeSeriesArgument{point}) {}

    WiringArgument(ScalarValue<> value) : value_{ScalarArgument{std::move(value)}} {}

    static WiringArgument absent_scalar(const ScalarType &type, const ScalarRuntimeType &runtime_type) {
        return WiringArgument{ScalarArgument{type, runtime_type}};
    }

    WiringArgument(GraphArgument graph) : value_(std::move(graph)) {}

    template <typename T>
        requires requires(T &&value) { WiringArgument{std::forward<T>(value)}; }
    WiringArgument(util::symbol name, T &&value) : WiringArgument{std::forward<T>(value)} {
        name_ = name;
    }

    template <typename T>
        requires details::graph_argument_facade<T>
    WiringArgument(const T &graph) : WiringArgument{graph.graph_argument()} {}

    template <typename T>
        requires details::is_typed_wiring_point<T>
    WiringArgument(T point) : WiringArgument{static_cast<WiringPoint>(point)} {
        std::get<TimeSeriesArgument>(value_).annotations = std::remove_cvref_t<T>::annotations;
    }

    template <typename T>
        requires(details::wiring_point_argument<T> && !details::is_typed_wiring_point<T> &&
                 !std::same_as<std::remove_cvref_t<T>, WiringPoint>)
    WiringArgument(const T &value) : WiringArgument{static_cast<WiringPoint>(value)} {}

    template <typename T>
        requires details::supported_scalar_argument<T>
    WiringArgument(T &&value) : value_(make_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires details::opaque_source_argument<T>
    explicit WiringArgument(T &&value)
        : value_(OpaqueScalarCandidate{ScalarArgument{cpp_scalar_type<Opaque<std::remove_cvref_t<T>>>(),
                                                      cpp_scalar_runtime_type<Opaque<std::remove_cvref_t<T>>>(),
                                                      std::addressof(value)}}) {}

    template <typename T>
        requires details::supported_optional_scalar_argument<T>
    WiringArgument(T &&value) : value_(make_optional_scalar_argument(std::forward<T>(value))) {}

    template <typename T>
        requires details::supported_scalar_value_argument<T>
    WiringArgument(const T &value)
        : value_([&] {
              using Scalar = typename details::scalar_value_argument<std::remove_cvref_t<T>>::scalar_type;
              const auto &type = scalar_declaration_traits<Scalar>::generic ? value.type() : cpp_scalar_type<Scalar>();
              return ScalarArgument{type, value.runtime_type(), nullptr, &details::scalar_data(value)};
          }()) {}

    template <typename T>
        requires details::graph_scalar_view_argument<Scalar<T>>
    WiringArgument(const Scalar<T> &value) : WiringArgument{ScalarValue<>{Scalar<>::from(value)}} {}

    template <util::compile_time_string Name, typename T>
        requires requires(T &value) { WiringArgument{value}; }
    WiringArgument(details::named_argument<Name, T> &value) : WiringArgument{value.value} {
        name_ = util::symbol{Name.view()};
        variadic_capture_mode_ = details::VariadicMode::none;
    }

    template <util::compile_time_string Name, typename T>
        requires requires(const T &value) { WiringArgument{value}; }
    WiringArgument(const details::named_argument<Name, T> &value) : WiringArgument{value.value} {
        name_ = util::symbol{Name.view()};
        variadic_capture_mode_ = details::VariadicMode::none;
    }

    template <util::compile_time_string Name, typename T>
        requires requires(T &&value) { WiringArgument{std::forward<T>(value)}; }
    WiringArgument(details::named_argument<Name, T> &&value) : WiringArgument{std::move(value.value)} {
        name_ = util::symbol{Name.view()};
        variadic_capture_mode_ = details::VariadicMode::none;
    }

    bool is_time_series() const noexcept { return std::holds_alternative<TimeSeriesArgument>(value_); }
    bool is_scalar() const noexcept {
        return std::holds_alternative<ScalarArgument>(value_) || std::holds_alternative<OpaqueScalarCandidate>(value_);
    }
    bool is_opaque_candidate() const noexcept { return std::holds_alternative<OpaqueScalarCandidate>(value_); }
    bool is_graph() const noexcept { return std::holds_alternative<GraphArgument>(value_); }
    bool requires_optional() const noexcept {
        const auto *time_series = std::get_if<TimeSeriesArgument>(&value_);
        return time_series != nullptr && time_series->annotations.optional;
    }
    details::WiringAnnotations annotations() const noexcept {
        const auto *time_series = std::get_if<TimeSeriesArgument>(&value_);
        return time_series == nullptr ? details::WiringAnnotations{} : time_series->annotations;
    }
    bool present() const noexcept {
        if (!is_scalar()) {
            return true;
        }
        const auto &scalar = scalar_argument();
        return scalar.owned.has_value() || scalar.data != nullptr || scalar.view != nullptr;
    }
    std::optional<util::symbol> name() const noexcept { return name_; }
    const Type &type() const noexcept {
        if (is_time_series()) {
            return *std::get<TimeSeriesArgument>(value_).resolution_type;
        }
        if (is_scalar()) {
            return scalar_type();
        }
        return graph().exact_type();
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

    const GraphArgument &graph() const {
        if (!is_graph()) {
            throw std::logic_error{"a wiring argument does not contain a graph"};
        }
        return std::get<GraphArgument>(value_);
    }

    static WiringArgument borrow(const WiringArgument &other) {
        auto result = WiringArgument{copy_value(other)};
        result.name_ = other.name_;
        result.variadic_capture_mode_ = other.variadic_capture_mode_;
        result.match_fixed_parameter_by_name_ = other.match_fixed_parameter_by_name_;
        return result;
    }

    void clear_name() noexcept { name_.reset(); }
    void set_name(util::symbol name) noexcept { name_ = name; }
    void require_optional() {
        if (!is_time_series()) {
            throw WiringError{"only a time-series wiring argument can require an optional input"};
        }
        std::get<TimeSeriesArgument>(value_).annotations.optional = true;
    }
    void require_valid_items() {
        if (!is_time_series()) {
            throw WiringError{"only a time-series wiring argument can require valid_items"};
        }
        std::get<TimeSeriesArgument>(value_).annotations.valid_items = true;
    }
    void replace_time_series(WiringPoint point) { std::get<TimeSeriesArgument>(value_).point = point; }
    void set_resolution_type(const TimeSeriesType &type) {
        if (!type.accepts(time_series().type())) {
            throw WiringError{"a wiring argument is incompatible with its resolution type"};
        }
        std::get<TimeSeriesArgument>(value_).resolution_type = &type;
    }

    details::VariadicMode variadic_capture_mode() const noexcept { return variadic_capture_mode_; }
    void clear_variadic_capture_mode() noexcept { variadic_capture_mode_ = details::VariadicMode::none; }
    void set_variadic_name(util::symbol name, details::VariadicMode mode) noexcept {
        name_ = name;
        variadic_capture_mode_ = mode;
    }
    void match_fixed_parameter_by_name() noexcept { match_fixed_parameter_by_name_ = true; }
    bool matches_fixed_parameter_by_name() const noexcept { return match_fixed_parameter_by_name_; }

    const ScalarRuntimeType &scalar_runtime_type() const noexcept {
        const auto &scalar = scalar_argument();
        return scalar.owned ? scalar.owned->runtime_type() : *scalar.runtime_type;
    }

    void bind_opaque_candidate(const Type &expected_type) {
        if (!is_opaque_candidate() || &scalar_type() != &expected_type) {
            throw WiringError{"an opaque C++ argument requires an exact matching fixed parameter"};
        }
        auto scalar = std::move(std::get<OpaqueScalarCandidate>(value_).scalar);
        value_.emplace<ScalarArgument>(std::move(scalar));
    }

    WiringArgument clone(allocator &memory) const {
        if (!is_scalar() || !scalar_has_value()) {
            return *this;
        }

        auto result = is_opaque_candidate()
                          ? WiringArgument{OpaqueScalarCandidate{ScalarArgument{scalar_type(), scalar_runtime_type()}}}
                          : WiringArgument{ScalarArgument{scalar_type(), scalar_runtime_type()}};
        result.scalar_argument().owned.emplace(scalar(), memory);
        result.name_ = name_;
        result.variadic_capture_mode_ = variadic_capture_mode_;
        result.match_fixed_parameter_by_name_ = match_fixed_parameter_by_name_;
        return result;
    }

  private:
    explicit WiringArgument(OpaqueScalarCandidate candidate) : value_(std::move(candidate)) {}

    ScalarArgument &scalar_argument() noexcept {
        if (auto *scalar = std::get_if<ScalarArgument>(&value_)) {
            return *scalar;
        }
        return std::get<OpaqueScalarCandidate>(value_).scalar;
    }
    const ScalarArgument &scalar_argument() const noexcept {
        if (const auto *scalar = std::get_if<ScalarArgument>(&value_)) {
            return *scalar;
        }
        return std::get<OpaqueScalarCandidate>(value_).scalar;
    }
    const ScalarType &scalar_type() const noexcept {
        const auto &scalar = scalar_argument();
        return scalar.type != nullptr ? *scalar.type : scalar.owned->type();
    }
    const void *scalar_data() const noexcept { return scalar_argument().data; }
    const details::ScalarData *scalar_view() const noexcept {
        const auto &scalar = scalar_argument();
        return scalar.owned ? &details::scalar_data(*scalar.owned) : scalar.view;
    }
    bool scalar_has_value() const noexcept { return scalar_data() != nullptr || scalar_view() != nullptr; }

    static std::variant<TimeSeriesArgument, ScalarArgument, OpaqueScalarCandidate, GraphArgument>
    copy_value(const WiringArgument &other) {
        if (other.is_time_series()) {
            return std::get<TimeSeriesArgument>(other.value_);
        }
        if (other.is_graph()) {
            return std::get<GraphArgument>(other.value_);
        }
        auto scalar = ScalarArgument{other.scalar_type(), other.scalar_runtime_type(), other.scalar_data(),
                                     other.scalar_view()};
        if (other.is_opaque_candidate()) {
            return OpaqueScalarCandidate{std::move(scalar)};
        }
        return scalar;
    }

    explicit WiringArgument(ScalarArgument scalar) : value_(std::move(scalar)) {}
    explicit WiringArgument(std::variant<TimeSeriesArgument, ScalarArgument, OpaqueScalarCandidate, GraphArgument> value)
        : value_(std::move(value)) {}

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

    std::variant<TimeSeriesArgument, ScalarArgument, OpaqueScalarCandidate, GraphArgument> value_;
    std::optional<util::symbol> name_;
    details::VariadicMode variadic_capture_mode_{details::VariadicMode::none};
    bool match_fixed_parameter_by_name_{};
};

} // namespace bg

namespace bg::details {

template <typename T>
concept graph_code_argument_value =
    wiring_point_argument<T> || supported_scalar_argument<T> || opaque_source_argument<T> ||
    supported_optional_scalar_argument<T> ||
    supported_scalar_value_argument<T> || graph_scalar_view_argument<T> ||
    std::same_as<std::remove_cvref_t<T>, GraphArgument> ||
    std::same_as<std::remove_cvref_t<T>, WiringArgument> ||
    std::same_as<std::remove_cvref_t<T>, WiringArgumentExpansion> || graph_argument_facade<T>;

template <typename T>
concept graph_code_argument = graph_code_argument_value<T> ||
                              (is_named_argument_v<T> && graph_code_argument_value<decltype(std::declval<T>().value)>);

BG_EXPORT void append_wiring_arguments(std::vector<WiringArgument> &result, WiringArgumentExpansion expansion);

// Bind generated arguments to a target signature before making an ordinary wiring call.
BG_EXPORT std::vector<WiringArgument> bind_wiring_arguments(const Signature &signature,
                                                            std::span<const WiringArgument> arguments);

template <typename Argument>
void append_wiring_arguments(std::vector<WiringArgument> &result, Argument &&argument) {
    result.emplace_back(std::forward<Argument>(argument));
}

template <typename... Arguments> std::vector<WiringArgument> make_wiring_arguments(Arguments &&...arguments) {
    auto result = std::vector<WiringArgument>{};
    result.reserve(sizeof...(Arguments));
    (append_wiring_arguments(result, std::forward<Arguments>(arguments)), ...);
    return result;
}

} // namespace bg::details

namespace bg {

template <details::wiring_point_argument Point> auto args(const Point &bundle) {
    constexpr auto annotations = [] {
        if constexpr (details::is_typed_wiring_point<Point>) {
            return std::remove_cvref_t<Point>::annotations;
        }
        return details::WiringAnnotations{};
    }();
    return details::WiringArgumentExpansion{static_cast<WiringPoint>(bundle), details::VariadicMode::positional,
                                            annotations};
}

template <details::wiring_point_argument Point> auto kwargs(const Point &bundle) {
    constexpr auto annotations = [] {
        if constexpr (details::is_typed_wiring_point<Point>) {
            return std::remove_cvref_t<Point>::annotations;
        }
        return details::WiringAnnotations{};
    }();
    return details::WiringArgumentExpansion{static_cast<WiringPoint>(bundle), details::VariadicMode::named,
                                            annotations};
}

inline WiringArgument opt(WiringArgument argument) {
    argument.require_optional();
    return argument;
}

template <typename Value>
    requires(details::supported_scalar_argument<Value> || details::supported_optional_scalar_argument<Value> ||
             details::supported_scalar_value_argument<Value>)
void opt(Value &&) = delete;

} // namespace bg
