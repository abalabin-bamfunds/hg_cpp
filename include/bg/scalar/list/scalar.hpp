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
        return Scalar<>::from(value_).visit(std::forward<Functions>(functions)...);
    }

    std::size_t hash() const noexcept
        requires details::scalar_capability_traits<List<T>>::supports_hash
    {
        return value_.hash();
    }

    bool equals(const details::ScalarData &other) const noexcept
        requires details::scalar_capability_traits<List<T>>::supports_equality
    {
        return value_.equals(other);
    }

    operator const details::ScalarData &() const & noexcept { return value_; }
    operator const details::ScalarData &() const && = delete;

    [[nodiscard]] static Scalar from(const details::ScalarData &value) noexcept { return Scalar{value}; }
    [[nodiscard]] static Scalar from(const ScalarRuntimeType &runtime_type, const void *data) noexcept {
        return Scalar{details::ScalarData{runtime_type, data}};
    }
    [[nodiscard]] static Scalar from(const details::scalar_stored_t<List<T>> &value) noexcept {
        return from(cpp_scalar_runtime_type<List<T>>(), std::addressof(value));
    }
    static Scalar from(details::scalar_stored_t<List<T>> &&) = delete;

    using scalar_type = List<T>;

  private:
    const ScalarListRuntimeType &runtime_type() const noexcept {
        return static_cast<const ScalarListRuntimeType &>(value_.runtime_type());
    }

    explicit Scalar(details::ScalarData value) noexcept : value_{value} {}
    details::ScalarData value_;
};

} // namespace bg
