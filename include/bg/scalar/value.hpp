#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/construction.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/type.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

namespace details {

class HierarchicalScalarValueFactory;

template <typename T, typename Source>
concept direct_scalar_source =
    (scalar_storage_constructible_from<T, Source> ||
     (scalar_storage_has_borrow_value<T> &&
      std::constructible_from<typename scalar_storage_traits<T>::value_type, Source &&>)) &&
    [] {
        if constexpr (hierarchical_scalar_handle<Source>) {
            return false;
        } else if constexpr (scalar_source<Source>) {
            return !std::same_as<canonical_scalar_t<Source>, T>;
        } else {
            return true;
        }
    }();

} // namespace details

template <typename T> class ScalarValue {
  public:
    [[nodiscard]] static ScalarValue from_construction(ScalarConstruction construction)
        requires(!details::generic_scalar<T>)
    {
        const auto &runtime_type = cpp_scalar_runtime_type<T>();
        return ScalarValue{details::allocate_scalar_value(runtime_type, nullptr, [&](void *destination,
                                                                                     allocator &memory) {
            construction.construct(construction.context, runtime_type, destination, memory);
        })};
    }

    [[nodiscard]] static ScalarValue from_construction(ScalarConstruction construction, allocator &memory)
        requires(!details::generic_scalar<T>)
    {
        const auto &runtime_type = cpp_scalar_runtime_type<T>();
        return ScalarValue{details::allocate_scalar_value(runtime_type, &memory, [&](void *destination,
                                                                                     allocator &resource) {
            construction.construct(construction.context, runtime_type, destination, resource);
        })};
    }

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
    ScalarValue(Source &&source, allocator &memory)
        : value_{make(details::make_scalar_value<T>(canonical_scalar(std::forward<Source>(source)), &memory))} {}

    template <typename Source>
        requires details::direct_scalar_source<T, Source>
    ScalarValue(Source &&source, allocator &memory)
        : value_{make(details::make_scalar_value<T>(std::forward<Source>(source), &memory))} {}

    explicit ScalarValue(const Scalar<T> &source)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), nullptr))} {}
    ScalarValue(const Scalar<T> &source, allocator &memory)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), &memory))} {}

    template <details::hierarchical_scalar_handle Source>
        requires details::hierarchical_scalar_declaration<T> &&
                 (!std::same_as<typename std::remove_cvref_t<Source>::scalar_type, T>)
    explicit ScalarValue(const Source &source)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), nullptr))} {}

    template <details::hierarchical_scalar_handle Source>
        requires details::hierarchical_scalar_declaration<T> &&
                 (!std::same_as<typename std::remove_cvref_t<Source>::scalar_type, T>)
    ScalarValue(const Source &source, allocator &memory)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), &memory))} {}

    ~ScalarValue() { release(); }

    ScalarValue(const ScalarValue &) = delete;
    ScalarValue &operator=(const ScalarValue &) = delete;

    ScalarValue(ScalarValue &&other) noexcept : value_{other.value_} { other.value_ = details::ScalarData{}; }

    ScalarValue &operator=(ScalarValue &&other) noexcept {
        release();
        value_ = other.value_;
        other.value_ = details::ScalarData{};
        return *this;
    }

    const ScalarType &type() const noexcept { return value_.type(); }
    const ScalarRuntimeType &runtime_type() const noexcept { return value_.runtime_type(); }

    std::size_t hash() const
        requires requires(const Scalar<T> &value) { value.hash(); }
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<T>::supports_equality
    bool equals(const Other &other) const {
        return value_.equals(details::scalar_data(other));
    }

    Scalar<T> view() const & noexcept { return Scalar<T>::from(*this); }
    Scalar<T> view() const && = delete;

    using scalar_type = T;

  private:
    friend class details::HierarchicalScalarValueFactory;

    explicit ScalarValue(details::OwnedScalarData value) noexcept : value_{make(value)} {}

    [[nodiscard]] static details::ScalarData make(details::OwnedScalarData value) noexcept {
        return {*value.runtime_type, value.data, value.mode};
    }

    void release() noexcept {
        details::release_scalar_value(value_);
        value_ = details::ScalarData{};
    }

    template <typename U> friend const details::ScalarData &details::scalar_data(const ScalarValue<U> &) noexcept;

    details::ScalarData value_;
};

template <> class ScalarValue<> {
  public:
    [[nodiscard]] static ScalarValue from_construction(const ScalarRuntimeType &runtime_type,
                                                       ScalarConstruction construction) {
        return ScalarValue{details::allocate_scalar_value(runtime_type, nullptr, [&](void *destination,
                                                                                     allocator &memory) {
            construction.construct(construction.context, runtime_type, destination, memory);
        })};
    }

    [[nodiscard]] static ScalarValue from_construction(const ScalarRuntimeType &runtime_type,
                                                       ScalarConstruction construction, allocator &memory) {
        return ScalarValue{details::allocate_scalar_value(runtime_type, &memory, [&](void *destination,
                                                                                     allocator &resource) {
            construction.construct(construction.context, runtime_type, destination, resource);
        })};
    }

    template <scalar_source Source>
    explicit ScalarValue(Source &&source)
        : value_{make(details::make_scalar_value<canonical_scalar_t<Source>>(
              canonical_scalar(std::forward<Source>(source)), nullptr))} {}

    template <typename T>
    explicit ScalarValue(const Scalar<T> &source)
        : value_{make(details::copy_scalar_value(details::scalar_data(source), nullptr))} {}

    template <typename T>
    ScalarValue(const Scalar<T> &source, allocator &memory)
        : value_{make(details::copy_scalar_value(details::scalar_data(source), &memory))} {}

    template <typename T>
    explicit ScalarValue(const ScalarValue<T> &source)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), nullptr))} {}

    template <typename T>
    ScalarValue(const ScalarValue<T> &source, allocator &memory)
        : value_{make(details::copy_scalar_value<T>(details::scalar_data(source), &memory))} {}

    ~ScalarValue() { release(); }

    ScalarValue(const ScalarValue &) = delete;

    ScalarValue &operator=(const ScalarValue &source) {
        if (this != &source) {
            auto replacement = ScalarValue{details::scalar_data(source)};
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

    const ScalarType &type() const noexcept { return value_.type(); }
    const ScalarRuntimeType &runtime_type() const noexcept { return value_.runtime_type(); }
    std::size_t hash() const { return value_.hash(); }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); }
    bool equals(const Other &other) const {
        return value_.equals(details::scalar_data(other));
    }

    Scalar<> view() const & noexcept { return Scalar<>::from(*this); }
    Scalar<> view() const && = delete;

    using scalar_type = void;

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const ScalarValue<U> &) noexcept;

    explicit ScalarValue(details::OwnedScalarData value) noexcept : value_{make(value)} {}

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

namespace details {

template <typename T> const ScalarData &scalar_data(const ScalarValue<T> &value) noexcept {
    return value.value_;
}

} // namespace details

template <scalar_source Source> ScalarValue(Source &&) -> ScalarValue<canonical_scalar_t<Source>>;
template <scalar_source Source>
ScalarValue(Source &&, allocator &) -> ScalarValue<canonical_scalar_t<Source>>;

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
        return scalar_handle_traits<ScalarType>::supports_equality;
    }
}();

template <typename T>
concept EqualityComparableScalarHandle = ScalarHandle<T> && scalar_handle_supports_equality<T>;

} // namespace details

template <details::EqualityComparableScalarHandle Left, details::EqualityComparableScalarHandle Right>
bool operator==(const Left &left, const Right &right) {
    using LeftScalar = typename std::remove_cvref_t<Left>::scalar_type;
    using RightScalar = typename std::remove_cvref_t<Right>::scalar_type;

    if constexpr (details::generic_scalar<LeftScalar>) {
        return left.equals(right);
    } else if constexpr (details::generic_scalar<RightScalar>) {
        return right.equals(left);
    } else if constexpr (details::hierarchical_scalar_declaration<LeftScalar> &&
                         details::hierarchical_scalar_declaration<RightScalar>) {
        return left.equals(right);
    } else if constexpr (std::same_as<LeftScalar, RightScalar>) {
        return left.equals(right);
    } else {
        return false;
    }
}

struct ScalarHash {
    using is_transparent = void;

    template <details::ScalarHandle Value> std::size_t operator()(const Value &value) const {
        return Scalar<>::from(value).hash();
    }
};

struct ScalarEqual {
    using is_transparent = void;

    template <details::ScalarHandle Left, details::ScalarHandle Right>
    bool operator()(const Left &left, const Right &right) const {
        return Scalar<>::from(left).equals(Scalar<>::from(right));
    }
};

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const Scalar<Source> &source) noexcept {
    return Scalar<Target>::from(source);
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> scalar_cast(const Scalar<Source> &source) {
    if (!cpp_scalar_type<Target>().accepts(source.type())) {
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
Scalar<Target> scalar_cast(const ScalarValue<Source> &&) = delete;

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
[[nodiscard]] Scalar<Target> unchecked_scalar_cast(const ScalarValue<Source> &source) noexcept {
    return unchecked_scalar_cast<Target>(source.view());
}

template <typename Target, typename Source>
    requires(!details::scalar_variable<Target>)
Scalar<Target> unchecked_scalar_cast(const ScalarValue<Source> &&) = delete;

static_assert(sizeof(Scalar<>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<SCALAR>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<HIERARCHICAL>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<STRUCT>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<List<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Set<SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<Dict<SCALAR, SCALAR>>) == 2 * sizeof(void *));
static_assert(sizeof(Scalar<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<std::int64_t>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<EvaluationTime>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<string>) == 2 * sizeof(void *));
static_assert(sizeof(ScalarValue<>) == 2 * sizeof(void *));

} // namespace bg
