#pragma once

#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/set/runtime_type.hpp>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace bg {

template <typename T> class Scalar<Set<T>> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t size() const noexcept { return runtime_type().item_count(value_.data()); }
    bool empty() const noexcept { return size() == 0; }
    bool contains(const Scalar<T> &item) const {
        return runtime_type().contains(value_.data(), item.input_view());
    }

    template <scalar_source Source>
        requires(!details::scalar_variable<T> && std::same_as<canonical_scalar_t<Source>, T>)
    bool contains(Source &&item) const {
        const auto value = canonical_scalar(std::forward<Source>(item));
        return runtime_type().contains(value_.data(), details::make_scalar_source<T>(value));
    }

    template <typename Function> void for_each(Function &&function) const {
        runtime_type().for_each(value_.data(), std::addressof(function),
                                [](void *context, ScalarSetRuntimeType::Item item) {
                                    auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
                                    std::invoke(callback, Scalar<T>::from(item.runtime_type, item.data));
                                });
    }

    const Scalar *operator->() const noexcept { return this; }
    const ScalarType &type() const noexcept { return value_.runtime_type().type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(value_).visit(std::forward<Functions>(functions)...);
    }

    std::size_t hash() const noexcept
        requires details::scalar_capability_traits<Set<T>>::supports_hash
    {
        return value_.hash();
    }

    bool equals(const details::ScalarData &other) const noexcept
        requires details::scalar_capability_traits<Set<T>>::supports_equality
    {
        return value_.equals(other);
    }

    const details::ScalarData &input_view() const & noexcept { return value_; }
    const details::ScalarData &input_view() const && = delete;

    [[nodiscard]] static Scalar from(const details::ScalarData &value) noexcept { return Scalar{value}; }
    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Set<T>> &value) noexcept {
        return from(cpp_scalar_runtime_type<Set<T>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<Set<T>> &&) = delete;

    using scalar_type = Set<T>;

  private:
    const ScalarSetRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarSetRuntimeType &>(value_.runtime_type());
    }

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
