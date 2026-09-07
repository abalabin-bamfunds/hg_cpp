#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/type.hpp>
#include <bg/type/type_map.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

template <typename T, typename Source>
concept direct_scalar_source = scalar_storage_constructible_from<T, Source> && [] {
    if constexpr (scalar_source<Source>) {
        return !std::same_as<canonical_scalar_t<Source>, T>;
    } else {
        return true;
    }
}();

} // namespace details

template <typename T> class ScalarValue {
    using View = details::ScalarData;

  public:
    template <scalar_source Source>
        requires std::same_as<canonical_scalar_t<Source>, T>
    ScalarValue(Source &&source)
        : value_{make(details::make_scalar_value<T>(canonical_scalar(std::forward<Source>(source)), nullptr))} {}

    template <typename Source>
        requires details::direct_scalar_source<T, Source>
    explicit ScalarValue(Source &&source)
        : value_{make(details::make_scalar_value<T>(std::forward<Source>(source), nullptr))} {}

    template <scalar_source Source>
        requires std::same_as<canonical_scalar_t<Source>, T>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_{make(details::make_scalar_value<T>(canonical_scalar(std::forward<Source>(source)), &memory))} {}

    template <typename Source>
        requires details::direct_scalar_source<T, Source>
    ScalarValue(Source &&source, std::pmr::memory_resource &memory)
        : value_{make(details::make_scalar_value<T>(std::forward<Source>(source), &memory))} {}

    explicit ScalarValue(const Scalar<T> &source)
        : value_{make(details::copy_scalar_value<T>(source.input_view(), nullptr))} {}
    ScalarValue(const Scalar<T> &source, std::pmr::memory_resource &memory)
        : value_{make(details::copy_scalar_value<T>(source.input_view(), &memory))} {}

    ~ScalarValue() { release(); }

    ScalarValue(const ScalarValue &) = delete;
    ScalarValue &operator=(const ScalarValue &) = delete;

    ScalarValue(ScalarValue &&other) noexcept : value_{other.value_} { other.value_ = View{}; }

    ScalarValue &operator=(ScalarValue &&other) noexcept {
        release();
        value_ = other.value_;
        other.value_ = View{};
        return *this;
    }

    Scalar<T> view() const & noexcept { return Scalar<T>::from(value_); }
    Scalar<T> view() const && = delete;
    const View &input_view() const & noexcept { return value_; }
    const View &input_view() const && = delete;

    using scalar_type = T;

  private:
    [[nodiscard]] static View make(details::OwnedScalarData value) noexcept {
        return {*value.runtime_type, value.data, value.mode};
    }

    void release() noexcept {
        details::release_scalar_value(value_);
        value_ = View{};
    }

    View value_;
};

template <> class ScalarValue<> {
  public:
    template <typename T>
    explicit ScalarValue(const Scalar<T> &source)
        : value_{make(details::copy_scalar_value(source.input_view(), nullptr))} {}

    template <typename T>
    ScalarValue(const Scalar<T> &source, std::pmr::memory_resource &memory)
        : value_{make(details::copy_scalar_value(source.input_view(), &memory))} {}

    template <typename T>
    explicit ScalarValue(const ScalarValue<T> &source)
        : value_{make(details::copy_scalar_value<T>(source.input_view(), nullptr))} {}

    template <typename T>
    ScalarValue(const ScalarValue<T> &source, std::pmr::memory_resource &memory)
        : value_{make(details::copy_scalar_value<T>(source.input_view(), &memory))} {}

    ~ScalarValue() { release(); }

    ScalarValue(const ScalarValue &) = delete;

    ScalarValue &operator=(const ScalarValue &source) {
        if (this != &source) {
            auto replacement = ScalarValue{source.input_view()};
            *this = std::move(replacement);
        }
        return *this;
    }

    template <typename T> ScalarValue &operator=(const Scalar<T> &source) {
        auto replacement = ScalarValue{source};
        *this = std::move(replacement);
        return *this;
    }

    template <typename T> ScalarValue &operator=(const ScalarValue<T> &source) {
        auto replacement = ScalarValue{source};
        *this = std::move(replacement);
        return *this;
    }

    ScalarValue(ScalarValue &&other) noexcept : value_{other.value_} { other.value_ = details::ScalarData{}; }

    ScalarValue &operator=(ScalarValue &&other) noexcept {
        release();
        value_ = other.value_;
        other.value_ = details::ScalarData{};
        return *this;
    }

    Scalar<> view() const & noexcept { return Scalar<>::from(value_); }
    Scalar<> view() const && = delete;
    const details::ScalarData &input_view() const & noexcept { return value_; }
    const details::ScalarData &input_view() const && = delete;

    using scalar_type = void;

  private:
    explicit ScalarValue(const details::ScalarData &source)
        : value_{make(details::copy_scalar_value(source, nullptr))} {}

    [[nodiscard]] static details::ScalarData make(details::OwnedScalarData value) noexcept {
        return {*value.runtime_type, value.data, value.mode};
    }

    void release() noexcept {
        details::release_scalar_value(value_);
        value_ = details::ScalarData{};
    }

    details::ScalarData value_;
};

template <scalar_source Source> ScalarValue(Source &&) -> ScalarValue<canonical_scalar_t<Source>>;
template <scalar_source Source>
ScalarValue(Source &&, std::pmr::memory_resource &) -> ScalarValue<canonical_scalar_t<Source>>;

namespace details {

template <typename T> struct scalar_handle : std::false_type {};
template <typename T> struct scalar_handle<Scalar<T>> : std::true_type {};
template <typename T> struct scalar_handle<ScalarValue<T>> : std::true_type {};

template <typename T>
concept ScalarHandle = scalar_handle<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool scalar_handle_supports_equality = [] {
    using ScalarType = typename std::remove_cvref_t<T>::scalar_type;
    if constexpr (std::same_as<ScalarType, void>) {
        return false;
    } else {
        return scalar_capability_traits<ScalarType>::supports_equality;
    }
}();

template <typename T>
concept EqualityComparableScalarHandle = ScalarHandle<T> && scalar_handle_supports_equality<T>;

} // namespace details

template <details::EqualityComparableScalarHandle Left, details::EqualityComparableScalarHandle Right>
bool operator==(const Left &left, const Right &right) noexcept {
    using LeftScalar = typename std::remove_cvref_t<Left>::scalar_type;
    using RightScalar = typename std::remove_cvref_t<Right>::scalar_type;

    if constexpr (details::generic_scalar<LeftScalar>) {
        return left.equals(right.input_view());
    } else if constexpr (details::generic_scalar<RightScalar>) {
        return right.equals(left.input_view());
    } else if constexpr (std::same_as<LeftScalar, RightScalar>) {
        return static_cast<const details::ScalarData &>(left.input_view())
            .equals(details::ScalarData{right.input_view()});
    } else {
        return false;
    }
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const Scalar<Source> &source) noexcept {
    return Scalar<Target>::from(source.input_view());
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const Scalar<Source> &source) {
    auto bindings = TypeMap{};
    if (!cpp_scalar_type<Target>().match(source.input_view().type(), bindings)) {
        throw std::bad_cast{};
    }
    return unchecked_scalar_cast<Target>(source);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const ScalarValue<Source> &source) {
    return scalar_cast<Target>(source.view());
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const ScalarValue<Source> &source) noexcept {
    return unchecked_scalar_cast<Target>(source.view());
}

static_assert(sizeof(Scalar<>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<SCALAR>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Set<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Dict<SCALAR, SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<EvaluationTime>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<string>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<>) == 2 * sizeof(void *));

} // namespace bg
