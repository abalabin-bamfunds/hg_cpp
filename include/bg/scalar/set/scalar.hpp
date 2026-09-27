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
        return runtime_type().contains(value_.data(), details::scalar_data(item));
    }

    template <typename Source>
        requires(!details::scalar_variable<T> && scalar_source_for<T, Source>)
    bool contains(Source &&item) const {
        const auto value = scalar_source_value<T>(std::forward<Source>(item));
        return runtime_type().contains(value_.data(), details::make_scalar_source<T>(value));
    }

    template <typename Function> void for_each(Function &&function) const {
        runtime_type().for_each(value_.data(), std::addressof(function),
                                [](void *context, ScalarSetRuntimeType::Item item) {
                                    auto &callback = *static_cast<std::remove_reference_t<Function> *>(context);
                                    std::invoke(callback, Scalar<T>::from(item.runtime_type, item.data));
                                });
    }

    const ScalarType &type() const noexcept { return value_.runtime_type().type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    std::size_t hash() const noexcept
        requires details::scalar_handle_traits<Set<T>>::supports_hash
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<Set<T>>::supports_equality
    bool equals(const Other &other) const noexcept {
        return value_.equals(details::scalar_data(other));
    }
    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }
    template <typename Source> [[nodiscard]] static Scalar from(const Scalar<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const Scalar<Source> &&) = delete;
    template <typename Source> [[nodiscard]] static Scalar from(const ScalarValue<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const ScalarValue<Source> &&) = delete;
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<Set<T>> &value) noexcept {
        return from(cpp_scalar_runtime_type<Set<T>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<Set<T>> &&) = delete;

    using scalar_type = Set<T>;

  public:
    const ScalarSetRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarSetRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
