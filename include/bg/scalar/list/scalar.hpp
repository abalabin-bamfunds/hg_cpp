#pragma once

#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/scalar.hpp>

namespace bg {

template <typename T> class Scalar<List<T>> {
  public:
    Scalar(const Scalar &) noexcept = default;
    Scalar &operator=(const Scalar &) noexcept = default;
    Scalar(Scalar &&) noexcept = default;
    Scalar &operator=(Scalar &&) noexcept = default;

    std::size_t size() const noexcept { return runtime_type().item_count(value_.data()); }
    bool empty() const noexcept { return size() == 0; }

    Scalar<T> operator[](std::size_t index) const {
        const auto &item_type = runtime_type().item_runtime_type();
        return Scalar<T>::from(item_type, runtime_type().item(value_.data(), index));
    }

    const ScalarType &type() const noexcept { return value_.runtime_type().type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return Scalar<>::from(*this).visit(std::forward<Functions>(functions)...);
    }

    std::size_t hash() const
        requires details::scalar_handle_traits<List<T>>::supports_hash
    {
        return value_.hash();
    }

    template <typename Other>
        requires requires(const Other &other) { details::scalar_data(other); } &&
                 details::scalar_handle_traits<List<T>>::supports_equality
    bool equals(const Other &other) const {
        return value_.equals(details::scalar_data(other));
    }
    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{runtime_type.borrow(data)};
    }
    template <typename Source> [[nodiscard]] static Scalar from(const Scalar<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const Scalar<Source> &&) = delete;
    template <typename Source> [[nodiscard]] static Scalar from(const ScalarValue<Source> &value) noexcept {
        return Scalar{details::scalar_data(value)};
    }
    template <typename Source> static Scalar from(const ScalarValue<Source> &&) = delete;
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<List<T>> &value) noexcept {
        return from(cpp_scalar_runtime_type<List<T>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<List<T>> &&) = delete;

    using scalar_type = List<T>;

  public:
    const ScalarListRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarListRuntimeType &>(value_.runtime_type());
    }
    const void *data() const noexcept { return value_.data(); }

  private:
    template <typename U> friend const details::ScalarData &details::scalar_data(const Scalar<U> &) noexcept;

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
